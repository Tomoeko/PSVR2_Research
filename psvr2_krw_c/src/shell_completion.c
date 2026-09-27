#include "shell_completion_internal.h"
#include "shell_internal.h"
#include "stage1_internal.h"

#include <ctype.h>
#include <dirent.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

#define COMPLETION_LIMIT 1024U
#define COMPLETION_PATH_LIMIT 4096U
#define COMPLETION_WORD_LIMIT 64U
#define COMPLETION_SCAN_LIMIT 4096U

typedef struct {
    char *words[COMPLETION_WORD_LIMIT];
    size_t count;
    size_t start, end;
    char *prefix;
    bool redirection;
} completion_input;

typedef struct {
    psvr2_line_completions *out;
    const char *directory_prefix;
    const char *name_prefix;
    bool directories_only;
    bool executable;
    bool overflow;
    size_t visited;
} directory_completion;

static bool registry_contains(psvr2_shell *shell, const char *name);

static bool native_quoting(psvr2_shell *shell, const completion_input *input)
{
    bool native = !shell->busybox_mode;
    size_t index = 0;
    while (index < input->count && !strcmp(input->words[index], "krw")) {
        native = true;
        ++index;
    }
    if (!native || index == input->count) return false;
    const char *command = input->words[index];
    if (!strcmp(command, "bb") || !strcmp(command, "s1exec") ||
        !strcmp(command, "sh") || !strcmp(command, "exec")) return false;
    return registry_contains(shell, command);
}

static void clear_items(psvr2_line_completions *out)
{
    for (size_t i = 0; i < out->count; ++i) free(out->items[i]);
    free(out->items);
    out->items = NULL;
    out->count = 0;
}

static void clear_input(completion_input *input)
{
    for (size_t i = 0; i < input->count; ++i) free(input->words[i]);
    free(input->prefix);
}

static bool safe_name(const char *name)
{
    if (!name || !*name || !strcmp(name, ".") || !strcmp(name, ".."))
        return false;
    for (const unsigned char *p = (const unsigned char *)name; *p; ++p)
        if (*p < 0x20 || *p == 0x7f) return false;
    return true;
}

static bool append_item(psvr2_line_completions *out, const char *text)
{
    for (size_t i = 0; i < out->count; ++i)
        if (!strcmp(out->items[i], text)) return true;
    if (out->count >= COMPLETION_LIMIT) return false;
    char *copy = psvr2_strdup(text);
    if (!copy) return false;
    char **items = realloc(out->items, (out->count + 1) * sizeof(*items));
    if (!items) {
        free(copy);
        return false;
    }
    out->items = items;
    out->items[out->count++] = copy;
    return true;
}

/* Decode shell quoting without expanding variables, substitutions or globs. */
static char *decode_word(const char *line, size_t start, size_t end, bool native)
{
    char *word = malloc(end - start + 1);
    if (!word) return NULL;
    size_t length = 0;
    char quote = 0;
    for (size_t i = start; i < end; ++i) {
        unsigned char ch = (unsigned char)line[i];
        if (ch < 0x20 || ch == 0x7f) goto invalid;
        if (!quote && (ch == '\'' || ch == '"')) {
            quote = (char)ch;
        } else if (quote && ch == (unsigned char)quote) {
            quote = 0;
        } else if (ch == '\\' && (native || quote != '\'')) {
            /* ash preserves \ before ordinary bytes inside double quotes;
             * native split_line deliberately strips it in every quote mode. */
            if (!native && quote == '"' && i + 1 < end &&
                !strchr("\\$`\"", line[i + 1])) {
                word[length++] = '\\';
                continue;
            }
            if (++i < end) {
                ch = (unsigned char)line[i];
                if (ch < 0x20 || ch == 0x7f) goto invalid;
                word[length++] = line[i];
            }
        } else {
            if (quote != '\'' && (ch == '$' || ch == '`')) goto invalid;
            if (!quote && (ch == '*' || ch == '?' || ch == '[')) goto invalid;
            word[length++] = (char)ch;
        }
    }
    word[length] = '\0';
    return word;
invalid:
    free(word);
    return NULL;
}

static bool is_operator(char ch)
{
    return ch && strchr(";&|<>", ch) != NULL;
}

static bool parse_input(psvr2_shell *shell, const char *line, size_t cursor,
                         completion_input *input)
{
    size_t length = strlen(line);
    if (length > COMPLETION_PATH_LIMIT || cursor > length) return false;
    size_t offset = 0;
    while (offset < length) {
        if (isspace((unsigned char)line[offset])) {
            if (offset >= cursor) break;
            ++offset;
            continue;
        }
        if (is_operator(line[offset])) {
            if (offset >= cursor) break;
            if (line[offset] == '<' || line[offset] == '>') {
                input->redirection = true;
            } else {
                for (size_t i = 0; i < input->count; ++i) free(input->words[i]);
                input->count = 0;
                input->redirection = false;
            }
            ++offset;
            continue;
        }
        size_t start = offset;
        bool native = native_quoting(shell, input);
        char quote = 0;
        while (offset < length) {
            char ch = line[offset];
            if (!quote && (isspace((unsigned char)ch) || is_operator(ch))) break;
            if (!quote && (ch == '\'' || ch == '"')) quote = ch;
            else if (quote && ch == quote) quote = 0;
            else if (ch == '\\' && (native || quote != '\'') && offset + 1 < length) ++offset;
            ++offset;
        }
        if (cursor >= start && cursor <= offset) {
            input->start = start;
            input->end = offset;
            input->prefix = decode_word(line, start, cursor, native);
            return input->prefix != NULL;
        }
        if (input->count >= COMPLETION_WORD_LIMIT) return false;
        char *word = decode_word(line, start, offset, native);
        if (!word) return false;
        input->words[input->count++] = word;
        input->redirection = false;
    }
    input->start = input->end = cursor;
    input->prefix = psvr2_strdup("");
    return input->prefix != NULL;
}

static bool append_quoted(psvr2_buffer *buffer, const char *text)
{
    if (!psvr2_buffer_append(buffer, "'", 1)) return false;
    for (const char *p = text; *p; ++p) {
        const char *fragment = *p == '\'' ? "'\\''" : p;
        size_t length = *p == '\'' ? 4 : 1;
        if (!psvr2_buffer_append(buffer, fragment, length)) return false;
    }
    return psvr2_buffer_append(buffer, "'", 1);
}

static bool trusted_busybox(const psvr2_shell *shell)
{
    return !strcmp(shell->busybox_path, "/tmp/busybox") ||
           !strcmp(shell->busybox_path, "/data/modules/busybox");
}

/* The script is fixed; all typed path/prefix bytes are positional data. */
static bool remote_query(
    psvr2_shell *shell, const char *script, const char *const *arguments,
    size_t argument_count, psvr2_buffer *output)
{
    if (shell->read_only || !shell->runtime ||
        (!trusted_busybox(shell) && !psvr2_shell_busybox_available(shell)) ||
        !trusted_busybox(shell)) return false;
    psvr2_buffer command = {0};
    bool ok = psvr2_shell_build_busybox_command(shell->busybox_path, script, &command);
    if (ok && command.len) --command.len; /* Replace the builder's NUL. */
    if (ok) ok = psvr2_buffer_append(&command, " x", 2);
    for (size_t i = 0; ok && i < argument_count; ++i)
        ok = psvr2_buffer_append(&command, " ", 1) && append_quoted(&command, arguments[i]);
    const char terminator = '\0';
    ok = ok && command.len + strlen(PSVR2_STAGE1_COMMAND_PREFIX) <
                   PSVR2_STAGE1_COMMAND_CAPACITY &&
         psvr2_buffer_append(&command, &terminator, 1);
    int64_t result = -1;
    if (ok)
        ok = psvr2_stage1_exec(shell->runtime, (const char *)command.data,
                               1.0, &result, output) && result == 0 &&
             output->len < PSVR2_STAGE1_OUTPUT_LIMIT;
    psvr2_buffer_free(&command);
    return ok;
}

static bool add_directory_item(directory_completion *context, const char *name,
                               bool directory, bool executable)
{
    if (!safe_name(name) || strchr(name, '/') ||
        strncmp(name, context->name_prefix, strlen(context->name_prefix)) ||
        (name[0] == '.' && context->name_prefix[0] != '.') ||
        (context->directories_only && !directory) ||
        (context->executable && !directory && !executable)) return true;
    size_t path_length = strlen(context->directory_prefix);
    size_t name_length = strlen(name);
    if (path_length + name_length + 2 > COMPLETION_PATH_LIMIT) return true;
    char text[COMPLETION_PATH_LIMIT];
    memcpy(text, context->directory_prefix, path_length);
    memcpy(text + path_length, name, name_length);
    size_t length = path_length + name_length;
    if (directory) text[length++] = '/';
    text[length] = '\0';
    if (!append_item(context->out, text)) {
        context->overflow = true;
        return false;
    }
    return true;
}

static bool vfs_entry(void *opaque, uint64_t dentry, const char *name,
                      psvr2_vfs_type type, uint64_t size)
{
    directory_completion *context = opaque;
    (void)dentry;
    (void)size;
    if (++context->visited > COMPLETION_SCAN_LIMIT) return false;
    return add_directory_item(context, name, type == PSVR2_VFS_DIRECTORY, false);
}

static bool complete_vfs(psvr2_shell *shell, uint64_t base, const char *directory,
                         directory_completion *context)
{
    if (!shell->vfs.krw || !base || context->executable) return false;
    uint64_t dentry = psvr2_vfs_resolve_path(&shell->vfs, base, directory);
    if (!dentry) return false;
    psvr2_walk_result result = psvr2_vfs_list_ex(&shell->vfs, dentry, vfs_entry, context);
    return result == PSVR2_WALK_COMPLETE && !context->overflow;
}

static bool complete_remote(psvr2_shell *shell, const char *directory,
                            directory_completion *context)
{
    static const char script[] =
        "i=0;for f in \"$1\"/* \"$1\"/.[!.]* \"$1\"/..?*;do "
        "[ -e \"$f\" ]||[ -L \"$f\" ]||continue;n=${f##*/};"
        "case \"$n\" in \"$2\"*) ;;*) continue;;esac;"
        "t=f;[ -x \"$f\" ]&&t=x;[ -d \"$f\" ]&&t=d;"
        "printf \"%s\\0%s\\0\" \"$t\" \"$n\";"
        "i=$((i+1));[ $i -ge 128 ]&&{ printf \"!\\0\";exit;};done;true";
    const char *arguments[] = {directory, context->name_prefix};
    psvr2_buffer output = {0};
    bool ok = remote_query(shell, script, arguments, PSVR2_ARRAY_LEN(arguments), &output);
    size_t offset = 0;
    while (ok && offset < output.len) {
        if (output.data[offset] == '!') { ok = false; break; }
        if (offset + 2 > output.len || output.data[offset + 1] != '\0') {
            ok = false;
            break;
        }
        char type = (char)output.data[offset];
        offset += 2;
        uint8_t *end = memchr(output.data + offset, '\0', output.len - offset);
        if (!end || (type != 'd' && type != 'f' && type != 'x')) {
            ok = false;
            break;
        }
        ok = add_directory_item(context, (const char *)output.data + offset,
                                 type == 'd', type == 'x');
        offset = (size_t)(end - output.data) + 1;
    }
    psvr2_buffer_free(&output);
    return ok && !context->overflow;
}

static bool complete_host(const char *directory, directory_completion *context)
{
    DIR *stream = opendir(directory);
    if (!stream) return false;
    struct dirent *entry;
    bool ok = true;
    while ((entry = readdir(stream)) != NULL) {
        if (++context->visited > COMPLETION_SCAN_LIMIT) { ok = false; break; }
        if (!safe_name(entry->d_name) ||
            strncmp(entry->d_name, context->name_prefix, strlen(context->name_prefix))) continue;
        char path[COMPLETION_PATH_LIMIT];
        int length = snprintf(path, sizeof(path), "%s/%s", directory, entry->d_name);
        struct stat status;
        if (length < 0 || (size_t)length >= sizeof(path) || stat(path, &status)) continue;
        if (!add_directory_item(context, entry->d_name, S_ISDIR(status.st_mode), false)) {
            ok = false;
            break;
        }
    }
    closedir(stream);
    return ok;
}

static bool complete_path(psvr2_shell *shell, const char *prefix, bool host,
                          bool native_vfs, const char *cwd, const char *fixed_directory,
                          bool directories_only, bool executable,
                          psvr2_line_completions *out)
{
    char expanded[COMPLETION_PATH_LIMIT];
    if (host && prefix[0] == '~' && (prefix[1] == '/' || !prefix[1])) {
        const char *home = getenv("HOME");
        if (!home) return false;
        int length = snprintf(expanded, sizeof(expanded), "%s%s", home, prefix + 1);
        if (length < 0 || (size_t)length >= sizeof(expanded)) return false;
        prefix = expanded;
    }
    const char *slash = strrchr(prefix, '/');
    size_t directory_length = slash ? (size_t)(slash - prefix) + 1 : 0;
    if (fixed_directory && slash) return false; /* Basename-only native argument. */
    char shown_directory[COMPLETION_PATH_LIMIT];
    memcpy(shown_directory, prefix, directory_length);
    shown_directory[directory_length] = '\0';
    const char *name_prefix = prefix + directory_length;
    char directory[COMPLETION_PATH_LIMIT];
    int length;
    if (fixed_directory)
        length = snprintf(directory, sizeof(directory), "%s", fixed_directory);
    else if (host || native_vfs || prefix[0] == '/')
        length = snprintf(directory, sizeof(directory), "%s", directory_length ? shown_directory : ".");
    else
        length = snprintf(directory, sizeof(directory), "%s/%s", cwd,
                           directory_length ? shown_directory : ".");
    if (length < 0 || (size_t)length >= sizeof(directory)) return false;
    directory_completion context = {
        .out = out, .directory_prefix = shown_directory, .name_prefix = name_prefix,
        .directories_only = directories_only, .executable = executable
    };
    if (host) return complete_host(directory, &context);
    if (shell->read_only)
        return native_vfs && complete_vfs(shell, shell->cwd, directory, &context);
    char vfs_directory[COMPLETION_PATH_LIMIT];
    memcpy(vfs_directory, directory, (size_t)length + 1);
    if (native_vfs && directory[0] != '/') {
        length = snprintf(directory, sizeof(directory), "%s/%s", cwd,
                           directory_length ? shown_directory : ".");
        if (length < 0 || (size_t)length >= sizeof(directory)) return false;
    }
    if (complete_remote(shell, directory, &context)) return true;
    /* Discard partial queries before the read-only cached VFS fallback. */
    clear_items(out);
    context.overflow = false;
    return native_vfs && complete_vfs(shell, shell->cwd, vfs_directory, &context);
}

static bool registry_contains(psvr2_shell *shell, const char *name)
{
    psvr2_line_completions commands = {0};
    bool found = false;
    if (psvr2_shell_complete_commands(shell, name, &commands))
        for (size_t i = 0; i < commands.count; ++i)
            if (!strcmp(commands.items[i], name)) found = true;
    clear_items(&commands);
    return found;
}

static bool complete_remote_commands(psvr2_shell *shell, const char *prefix,
                                     psvr2_line_completions *out)
{
    static const char *const builtins[] = {
        "cd", "echo", "exit", "export", "pwd", "read", "set", "unset", "wait"
    };
    for (size_t i = 0; i < PSVR2_ARRAY_LEN(builtins); ++i)
        if (!strncmp(builtins[i], prefix, strlen(prefix)) &&
            !append_item(out, builtins[i])) return false;
    static const char path_script[] =
        "i=0;IFS=:;for d in $PATH;do for f in \"${d:-.}\"/*;do "
        "[ -f \"$f\" ]&&[ -x \"$f\" ]||continue;n=${f##*/};"
        "case \"$n\" in \"$1\"*) printf \"%s\\0\" \"$n\";;*) continue;;esac;"
        "i=$((i+1));[ $i -ge 1024 ]&&{ printf \"!\\0\";exit;};done;done;true";
    const char *arguments[] = {prefix};
    psvr2_buffer output = {0};
    if (remote_query(shell, path_script, arguments, 1, &output)) {
        size_t offset = 0;
        while (offset < output.len) {
            uint8_t *end = memchr(output.data + offset, '\0', output.len - offset);
            if (!end || (output.data[offset] == '!' && end == output.data + offset + 1)) {
                psvr2_buffer_free(&output);
                return false;
            }
            const char *name = (const char *)output.data + offset;
            if (safe_name(name) && !strchr(name, '/') &&
                !strncmp(name, prefix, strlen(prefix)) && !append_item(out, name)) {
                psvr2_buffer_free(&output);
                return false;
            }
            offset = (size_t)(end - output.data) + 1;
        }
    }
    psvr2_buffer_free(&output);
    /* Applet names come from the actual selected binary, not a stale host list. */
    if (!shell->read_only && trusted_busybox(shell)) {
        char command[96];
        int length = snprintf(command, sizeof(command), "%s --list", shell->busybox_path);
        int64_t result = -1;
        if (length > 0 && (size_t)length < sizeof(command) &&
            psvr2_stage1_exec(shell->runtime, command, 1.0, &result, &output) &&
            result == 0 && output.len < PSVR2_STAGE1_OUTPUT_LIMIT) {
            size_t offset = 0;
            while (offset < output.len) {
                uint8_t *end = memchr(output.data + offset, '\n', output.len - offset);
                size_t size = end ? (size_t)(end - output.data) - offset : output.len - offset;
                char name[256];
                if (size && size < sizeof(name)) {
                    memcpy(name, output.data + offset, size);
                    name[size] = '\0';
                    if (safe_name(name) && !strchr(name, '/') &&
                        !strncmp(name, prefix, strlen(prefix)) && !append_item(out, name)) {
                        psvr2_buffer_free(&output);
                        return false;
                    }
                }
                offset += size + (end ? 1U : 0U);
            }
        }
        psvr2_buffer_free(&output);
    }
    return true;
}

static bool native_argument(psvr2_shell *shell, const char *command, size_t argument,
                             const completion_input *input, psvr2_line_completions *out)
{
    bool host =
        (!strcmp(command, "fast_upload")) ||
        (argument == 1 && (!strcmp(command, "upload") || !strcmp(command, "stage1") ||
                           !strcmp(command, "stage1_install") || !strcmp(command, "stage1_verify") ||
                           !strcmp(command, "get_all"))) ||
        (argument == 2 && !strcmp(command, "fast_download")) ||
        (argument == 3 && (!strcmp(command, "dumpmem") || !strcmp(command, "emmc_plain"))) ||
        (argument == 4 && !strcmp(command, "dumpuser"));
    if (!strcmp(command, "get") && argument >= 2) {
        uint64_t ignored;
        size_t first = input->count - argument + 1;
        bool numeric = first < input->count && psvr2_parse_u64(input->words[first], &ignored);
        host = argument == (numeric ? 3U : 2U);
    }
    bool vfs = argument == 1 && (!strcmp(command, "cd") || !strcmp(command, "ls") ||
                                 !strcmp(command, "cat") || !strcmp(command, "get"));
    const char *fixed = NULL;
    if (argument == 1 && !strcmp(command, "persist")) fixed = "/tmp";
    if (argument == 1 && (!strcmp(command, "unpersist") || !strcmp(command, "persist_backups")))
        fixed = "/data/modules";
    if (argument == 1 && !strcmp(command, "emmc")) fixed = "/dev";
    bool remote = (argument == 1 && (!strcmp(command, "fast_download") ||
                                     !strcmp(command, "backdoor"))) ||
                  (argument == 2 && !strcmp(command, "chmod"));
    if (!host && !vfs && !fixed && !remote) return true;
    return complete_path(shell, input->prefix, host, vfs, vfs ? shell->cwd_name : "/",
                          fixed, !strcmp(command, "cd") || !strcmp(command, "get_all"),
                          false, out);
}

static int compare_items(const void *left, const void *right)
{
    return strcmp(*(const char *const *)left, *(const char *const *)right);
}

static bool escape_items(const char *line, psvr2_line_completions *out)
{
    if (out->count > 1) qsort(out->items, out->count, sizeof(*out->items), compare_items);
    size_t write = 0;
    for (size_t i = 0; i < out->count; ++i) {
        if (write && !strcmp(out->items[write - 1], out->items[i])) free(out->items[i]);
        else out->items[write++] = out->items[i];
    }
    out->count = write;
    for (size_t i = 0; i < out->count; ++i) {
        const char *raw = out->items[i];
        size_t length = strlen(raw);
        bool space = out->count == 1 && length && raw[length - 1] != '/' &&
                     !isspace((unsigned char)line[out->end]);
        char *escaped = malloc(length * 2 + 2);
        if (!escaped) return false;
        size_t offset = 0;
        for (const unsigned char *p = (const unsigned char *)raw; *p; ++p) {
            if (*p < 0x80 && !isalnum(*p) && !strchr("_-/.:@+%,", *p))
                escaped[offset++] = '\\';
            escaped[offset++] = (char)*p;
        }
        if (space) escaped[offset++] = ' ';
        escaped[offset] = '\0';
        free(out->items[i]);
        out->items[i] = escaped;
    }
    return true;
}

bool psvr2_shell_complete(void *context, const char *line, size_t cursor,
                           psvr2_line_completions *out)
{
    if (!context || !line || !out) return false;
    memset(out, 0, sizeof(*out));
    psvr2_shell *shell = context;
    completion_input input = {0};
    bool ok = parse_input(shell, line, cursor, &input);
    if (!ok) { clear_input(&input); return false; }
    out->start = input.start;
    out->end = input.end;
    size_t command_index = 0;
    bool native = !shell->busybox_mode;
    const char *cwd = shell->busybox_mode ? shell->busybox_cwd : "/";
    while (command_index < input.count && !strcmp(input.words[command_index], "krw")) {
        native = true;
        ++command_index;
    }
    if (native && command_index < input.count &&
        (!strcmp(input.words[command_index], "bb") || !strcmp(input.words[command_index], "s1exec") ||
         !strcmp(input.words[command_index], "sh") || !strcmp(input.words[command_index], "exec"))) {
        native = false;
        cwd = "/";
        ++command_index;
    }
    if (input.redirection) {
        ok = complete_path(shell, input.prefix, false, false, cwd, NULL, false, false, out);
    } else if (command_index == input.count) {
        if (strchr(input.prefix, '/'))
            ok = complete_path(shell, input.prefix, false, false, cwd, NULL, false, true, out);
        else {
            if (native) ok = psvr2_shell_complete_commands(shell, input.prefix, out);
            else if (!strncmp("krw", input.prefix, strlen(input.prefix))) ok = append_item(out, "krw");
            if (ok && !shell->read_only) ok = complete_remote_commands(shell, input.prefix, out);
        }
    } else {
        const char *command = input.words[command_index];
        size_t argument = input.count - command_index;
        if (native && registry_contains(shell, command))
            ok = native_argument(shell, command, argument, &input, out);
        else
            ok = complete_path(shell, input.prefix, false, false, cwd, NULL,
                               !strcmp(command, "cd"), false, out);
    }
    if (ok) ok = escape_items(line, out);
    clear_input(&input);
    if (!ok) clear_items(out);
    return ok;
}
