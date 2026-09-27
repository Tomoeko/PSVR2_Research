#ifndef _XOPEN_SOURCE
#define _XOPEN_SOURCE 700
#endif

#include "line_editor_internal.h"

#include <errno.h>
#include <stdlib.h>
#include <string.h>

#if !defined(_WIN32)
#include <locale.h>
#if defined(__APPLE__)
#include <xlocale.h>
#endif
#include <sys/ioctl.h>
#include <sys/select.h>
#include <wchar.h>
#include <wctype.h>
#include <termios.h>
#include <unistd.h>
#endif

typedef enum {
    EDIT_KEY_NONE,
    EDIT_KEY_UP,
    EDIT_KEY_DOWN,
    EDIT_KEY_LEFT,
    EDIT_KEY_RIGHT,
    EDIT_KEY_WORD_LEFT,
    EDIT_KEY_WORD_RIGHT,
    EDIT_KEY_HOME,
    EDIT_KEY_END,
    EDIT_KEY_DELETE,
    EDIT_KEY_PASTE_BEGIN,
    EDIT_KEY_PASTE_END
} edit_key;

void psvr2_line_editor_init(psvr2_line_editor *editor) {
    if (editor) memset(editor, 0, sizeof(*editor));
}

void psvr2_line_editor_destroy(psvr2_line_editor *editor) {
    if (!editor) return;
    for (size_t index = 0; index < editor->count; ++index)
        free(editor->entries[index]);
    free(editor->draft);
    memset(editor, 0, sizeof(*editor));
}

void psvr2_line_completions_destroy(psvr2_line_completions *completions) {
    if (!completions) return;
    for (size_t index = 0; index < completions->count; ++index)
        free(completions->items[index]);
    free(completions->items);
    memset(completions, 0, sizeof(*completions));
}

bool psvr2_line_editor_add_history(
    psvr2_line_editor *editor, const char *line) {
    if (!editor || !line) return false;
    if (!*line) {
        editor->position = editor->count;
        return true;
    }
    if (editor->count &&
        strcmp(editor->entries[editor->count - 1], line) == 0) {
        editor->position = editor->count;
        return true;
    }

    char *copy = psvr2_strdup(line);
    if (!copy) return false;
    if (editor->count == PSVR2_LINE_HISTORY_LIMIT) {
        free(editor->entries[0]);
        memmove(
            editor->entries, editor->entries + 1,
            (PSVR2_LINE_HISTORY_LIMIT - 1) *
                sizeof(editor->entries[0]));
        --editor->count;
    }
    editor->entries[editor->count++] = copy;
    editor->position = editor->count;
    return true;
}

const char *psvr2_line_editor_previous(
    psvr2_line_editor *editor, const char *current) {
    if (!editor || !editor->count)
        return current ? current : "";
    if (editor->position == editor->count) {
        free(editor->draft);
        editor->draft = psvr2_strdup(current ? current : "");
        if (!editor->draft) return current ? current : "";
    }
    if (editor->position > 0) --editor->position;
    return editor->entries[editor->position];
}

const char *psvr2_line_editor_next(psvr2_line_editor *editor) {
    if (!editor) return "";
    if (editor->position < editor->count) ++editor->position;
    if (editor->position == editor->count)
        return editor->draft ? editor->draft : "";
    return editor->entries[editor->position];
}

static bool reserve_line(char **line, size_t *capacity, size_t needed) {
    if (needed == SIZE_MAX) return false;
    if (needed + 1 <= *capacity) return true;
    size_t next = *capacity ? *capacity : 64U;
    while (next < needed + 1) {
        if (next > SIZE_MAX / 2U) return false;
        next *= 2U;
    }
    char *resized = realloc(*line, next);
    if (!resized) return false;
    *line = resized;
    *capacity = next;
    return true;
}

static bool replace_line(char **line, size_t *length, size_t *capacity,
                         size_t *cursor, const char *replacement) {
    size_t replacement_length = strlen(replacement);
    if (!reserve_line(line, capacity, replacement_length))
        return false;
    memcpy(*line, replacement, replacement_length + 1);
    *length = replacement_length;
    *cursor = replacement_length;
    return true;
}

static bool read_stream_line(const char *prompt, char **line_out) {
    fputs(prompt, stdout);
    fflush(stdout);

    char *line = NULL;
    size_t length = 0;
    size_t capacity = 0;
    int character;
    while ((character = fgetc(stdin)) != EOF && character != '\n') {
        if (!reserve_line(&line, &capacity, length + 1)) {
            free(line);
            return false;
        }
        line[length++] = (char)character;
    }
    if (character == EOF && !length) {
        free(line);
        return false;
    }
    if (length && line[length - 1] == '\r') --length;
    if (!reserve_line(&line, &capacity, length)) {
        free(line);
        return false;
    }
    line[length] = '\0';
    *line_out = line;
    return true;
}

#if !defined(_WIN32)
static bool read_byte(psvr2_line_editor *editor, unsigned char *byte) {
    if (editor->input_position == editor->input_length) {
        ssize_t received;
        do {
            received = read(STDIN_FILENO, editor->input,
                            sizeof(editor->input));
        } while (received < 0 && errno == EINTR);
        if (received <= 0) return false;
        editor->input_position = 0;
        editor->input_length = (size_t)received;
    }
    *byte = editor->input[editor->input_position++];
    return true;
}

static bool read_byte_with_timeout(psvr2_line_editor *editor,
                                   unsigned char *byte) {
    if (editor->input_position < editor->input_length)
        return read_byte(editor, byte);
    for (;;) {
        fd_set read_set;
        FD_ZERO(&read_set);
        FD_SET(STDIN_FILENO, &read_set);
        struct timeval timeout = {.tv_sec = 0, .tv_usec = 50000};
        int ready = select(
            STDIN_FILENO + 1, &read_set, NULL, NULL, &timeout);
        if (ready < 0 && errno == EINTR) continue;
        return ready > 0 && read_byte(editor, byte);
    }
}

static edit_key read_escape_sequence(psvr2_line_editor *editor) {
    unsigned char byte;
    if (!read_byte_with_timeout(editor, &byte)) return EDIT_KEY_NONE;
    bool meta = byte == 27;
    if (meta && !read_byte_with_timeout(editor, &byte)) return EDIT_KEY_NONE;
    if (byte == 'b') return EDIT_KEY_WORD_LEFT;
    if (byte == 'f') return EDIT_KEY_WORD_RIGHT;
    if (byte != '[' && byte != 'O') return EDIT_KEY_NONE;

    /* Consume the complete CSI, including unsupported modifiers/keys. Leaving
     * its suffix in the input buffer turns bracketed paste into literal 00~. */
    char parameters[32];
    size_t length = 0;
    bool overflow = false;
    while (read_byte_with_timeout(editor, &byte)) {
        if (byte >= 0x40 && byte <= 0x7e) {
            if (overflow) return EDIT_KEY_NONE;
            parameters[length] = '\0';
            /* Terminal.app sends ESC b/f by default. Other terminal profiles
             * encode Option (or Control) as an xterm arrow modifier. */
            bool plain = !*parameters || !strcmp(parameters, "1");
            bool word_modifier = !strcmp(parameters, "1;3") ||
                                 !strcmp(parameters, "1;5") ||
                                 !strcmp(parameters, "1;9");
            bool word = (meta && plain) || word_modifier;
            if (meta && (!plain || (byte != 'C' && byte != 'D')))
                return EDIT_KEY_NONE;
            if (byte != '~' && !plain &&
                !((byte == 'C' || byte == 'D') && word_modifier))
                return EDIT_KEY_NONE;
            switch (byte) {
                case 'A': return EDIT_KEY_UP;
                case 'B': return EDIT_KEY_DOWN;
                case 'C': return word ? EDIT_KEY_WORD_RIGHT : EDIT_KEY_RIGHT;
                case 'D': return word ? EDIT_KEY_WORD_LEFT : EDIT_KEY_LEFT;
                case 'F': return EDIT_KEY_END;
                case 'H': return EDIT_KEY_HOME;
                case '~': {
                    char *end;
                    unsigned long number = strtoul(parameters, &end, 10);
                    if (*end) return EDIT_KEY_NONE;
                    switch (number) {
                        case 1: case 7: return EDIT_KEY_HOME;
                        case 3: return EDIT_KEY_DELETE;
                        case 4: case 8: return EDIT_KEY_END;
                        case 200: return EDIT_KEY_PASTE_BEGIN;
                        case 201: return EDIT_KEY_PASTE_END;
                        default: return EDIT_KEY_NONE;
                    }
                }
                default: return EDIT_KEY_NONE;
            }
        }
        if (length < sizeof(parameters) - 1)
            parameters[length++] = (char)byte;
        else
            overflow = true;
    }
    return EDIT_KEY_NONE;
}

static size_t previous_character(const char *line, size_t cursor) {
    if (!cursor) return 0;
    --cursor;
    while (cursor && ((unsigned char)line[cursor] & 0xc0U) == 0x80U)
        --cursor;
    return cursor;
}

static size_t next_character(const char *line, size_t length, size_t cursor) {
    if (cursor == length) return cursor;
    ++cursor;
    while (cursor < length &&
           ((unsigned char)line[cursor] & 0xc0U) == 0x80U)
        ++cursor;
    return cursor;
}

static bool word_character(const char *line, size_t length, size_t cursor) {
    /* The default macOS interactive shell is zsh. Its WORDCHARS make paths
     * and punctuation such as '-' and '_' part of a word. */
    static const char wordchars[] = "*?_-.[]~=/&;!#$%^(){}<>";
    unsigned char byte = (unsigned char)line[cursor];
    if (byte < 0x80U && strchr(wordchars, byte)) return true;
    mbstate_t state = {0};
    wchar_t character;
    size_t bytes = mbrtowc(&character, line + cursor, length - cursor, &state);
    return bytes != (size_t)-1 && bytes != (size_t)-2 && bytes &&
           (iswalnum((wint_t)character) != 0 ||
            (character != 0 && wcwidth(character) == 0));
}

static size_t previous_word(const char *line, size_t length, size_t cursor) {
    while (cursor) {
        size_t previous = previous_character(line, cursor);
        if (word_character(line, length, previous)) break;
        cursor = previous;
    }
    while (cursor) {
        size_t previous = previous_character(line, cursor);
        if (!word_character(line, length, previous)) break;
        cursor = previous;
    }
    return cursor;
}

static size_t next_word(const char *line, size_t length, size_t cursor) {
    while (cursor < length && word_character(line, length, cursor))
        cursor = next_character(line, length, cursor);
    while (cursor < length && !word_character(line, length, cursor))
        cursor = next_character(line, length, cursor);
    return cursor;
}

static size_t character_columns(const char *text, size_t remaining,
                                size_t *bytes) {
    mbstate_t state = {0};
    wchar_t character;
    size_t count = mbrtowc(&character, text, remaining, &state);
    if (count == (size_t)-1 || count == (size_t)-2 || !count) {
        *bytes = 1;
        return 1;
    }
    *bytes = count;
    int width = wcwidth(character);
    return width < 0 ? 1U : (size_t)width;
}

static size_t text_columns(const char *text, size_t length) {
    size_t columns = 0;
    for (size_t offset = 0; offset < length;) {
        size_t bytes;
        columns += character_columns(text + offset, length - offset, &bytes);
        offset += bytes;
    }
    return columns;
}

static size_t write_visible_text(const char *text, size_t length,
                                 size_t available) {
    size_t columns = 0;
    for (size_t offset = 0; offset < length;) {
        size_t bytes;
        size_t width = character_columns(text + offset, length - offset, &bytes);
        if (width > available - columns) break;
        /* Keep invalid/incomplete multibyte input and controls from changing
         * terminal geometry. The original bytes remain in the command. */
        if (bytes == 1 && ((unsigned char)text[offset] < 0x20U ||
                          (unsigned char)text[offset] >= 0x7fU))
            fputc('?', stdout);
        else
            fwrite(text + offset, 1, bytes, stdout);
        columns += width;
        offset += bytes;
    }
    return columns;
}

static void redraw_line(const char *prompt, const char *line,
                        size_t length, size_t cursor) {
    struct winsize size = {0};
    size_t columns = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
        columns = size.ws_col;
    /* Leave the last terminal cell unused so automatic wrapping never moves
     * the origin of the next redraw. Scroll long input horizontally instead. */
    size_t available = columns - 1;
    size_t prompt_length = strlen(prompt);
    size_t prompt_columns = text_columns(prompt, prompt_length);
    size_t prompt_budget = available / 2;
    if (prompt_columns > prompt_budget) {
        while (prompt_length && prompt_columns > prompt_budget) {
            size_t bytes;
            prompt_columns -= character_columns(prompt, prompt_length, &bytes);
            prompt += bytes;
            prompt_length -= bytes;
        }
    }
    size_t line_budget = available - prompt_columns;
    size_t cursor_columns = text_columns(line, cursor);
    size_t start = 0, hidden_columns = 0;
    while (start < cursor && cursor_columns - hidden_columns > line_budget) {
        size_t bytes;
        hidden_columns += character_columns(line + start, length - start, &bytes);
        start += bytes;
    }

    fputc('\r', stdout);
    write_visible_text(prompt, prompt_length, prompt_budget);
    write_visible_text(line + start, length - start, line_budget);
    fputs("\x1b[K\r", stdout);
    size_t cursor_column = prompt_columns + cursor_columns - hidden_columns;
    if (cursor_column)
        fprintf(stdout, "\x1b[%zuC", cursor_column);
    fflush(stdout);
}

static void show_completions(const psvr2_line_completions *completions) {
    struct winsize size = {0};
    size_t columns = 80;
    if (ioctl(STDOUT_FILENO, TIOCGWINSZ, &size) == 0 && size.ws_col > 0)
        columns = size.ws_col;
    fputs("\r\n", stdout);
    for (size_t index = 0; index < completions->count; ++index) {
        const char *item = completions->items[index];
        write_visible_text(item, strlen(item), columns - 1);
        fputs("\r\n", stdout);
    }
}

static bool complete_line(psvr2_line_editor *editor, const char *prompt,
                          char **line, size_t *length, size_t *capacity,
                          size_t *cursor, bool repeat, bool *dirty) {
    psvr2_line_completions completions = {0};
    bool ok = true;
    if (!editor->complete ||
        !editor->complete(editor->complete_context, *line, *cursor,
                          &completions) ||
        !completions.items || !completions.count ||
        completions.start > *cursor || completions.end < *cursor ||
        completions.end > *length) {
        fputc('\a', stdout);
        fflush(stdout);
        goto done;
    }
    size_t common = strlen(completions.items[0]);
    for (size_t index = 1; index < completions.count; ++index) {
        size_t offset = 0;
        while (offset < common && completions.items[index][offset] &&
               completions.items[0][offset] == completions.items[index][offset])
            ++offset;
        common = offset;
    }
    /* Do not insert the first half of a multi-byte character shared by names. */
    while (common &&
           ((unsigned char)completions.items[0][common] & 0xc0U) == 0x80U)
        --common;
    /* An incomplete escape would change the meaning of the next typed byte. */
    if (completions.count > 1) {
        size_t start = common;
        while (start && completions.items[0][start - 1] == '\\') --start;
        if ((common - start) % 2) --common;
    }
    size_t typed = *cursor - completions.start;
    /* Providers can normalize quotes to escapes, so byte lengths alone do
     * not indicate whether the candidate prefix extends the input. */
    bool advance = completions.count == 1 ||
                   (common && (common != typed ||
                    memcmp(*line + completions.start, completions.items[0], common)));
    if (advance) {
        size_t tail = *length - completions.end;
        if (completions.start > SIZE_MAX - common ||
            completions.start + common > SIZE_MAX - tail) {
            ok = false;
            goto done;
        }
        size_t next_length = completions.start + common + tail;
        if (!reserve_line(line, capacity, next_length)) {
            ok = false;
            goto done;
        }
        memmove(*line + completions.start + common, *line + completions.end,
                tail + 1);
        memcpy(*line + completions.start, completions.items[0], common);
        *length = next_length;
        *cursor = completions.start + common;
        *dirty = true;
    } else if (repeat && completions.count > 1) {
        show_completions(&completions);
        redraw_line(prompt, *line, *length, *cursor);
        *dirty = false;
    } else {
        fputc('\a', stdout);
        fflush(stdout);
    }
done:
    psvr2_line_completions_destroy(&completions);
    return ok;
}

static bool read_terminal_line(psvr2_line_editor *editor,
                               const char *prompt, char **line_out) {
    struct termios original;
    if (tcgetattr(STDIN_FILENO, &original) != 0)
        return read_stream_line(prompt, line_out);
    struct termios raw = original;
    raw.c_lflag &= (tcflag_t)~(ICANON | ECHO);
    raw.c_iflag &= (tcflag_t)~(ICRNL | IXON);
    raw.c_cc[VMIN] = 1;
    raw.c_cc[VTIME] = 0;
    if (tcsetattr(STDIN_FILENO, TCSANOW, &raw) != 0)
        return read_stream_line(prompt, line_out);

    locale_t text_locale = newlocale(LC_CTYPE_MASK, "", (locale_t)0);
    locale_t previous_locale = (locale_t)0;
    if (text_locale)
        previous_locale = uselocale(text_locale);
    fputs("\x1b[?2004h", stdout);
    char *line = NULL;
    size_t length = 0;
    size_t capacity = 0;
    size_t cursor = 0;
    bool accepted = false;
    bool eof = false;
    bool paste = false, pasted_cr = false, dirty = false;
    bool previous_tab = false;
    editor->position = editor->count;
    free(editor->draft);
    editor->draft = NULL;

    if (!reserve_line(&line, &capacity, 0)) goto done;
    line[0] = '\0';
    redraw_line(prompt, line, length, cursor);
    for (;;) {
        if (dirty && !paste &&
            editor->input_position == editor->input_length) {
            redraw_line(prompt, line, length, cursor);
            dirty = false;
        }
        unsigned char byte;
        if (!read_byte(editor, &byte)) {
            eof = true;
            break;
        }
        if (!paste && (byte == '\r' || byte == '\n')) {
            redraw_line(prompt, line, length, length);
            fputs("\r\n", stdout);
            accepted = true;
            break;
        }
        if (paste && byte != 27) {
            if (byte == '\r' || byte == '\n' || byte == '\t') {
                if (byte == '\n' && pasted_cr) {
                    pasted_cr = false;
                    continue;
                }
                pasted_cr = byte == '\r';
                byte = ' ';
            } else {
                pasted_cr = false;
            }
            if (byte < 0x20 || byte == 0x7f) continue;
            if (!reserve_line(&line, &capacity, length + 1)) goto done;
            memmove(line + cursor + 1, line + cursor, length - cursor + 1);
            line[cursor++] = (char)byte;
            ++length;
            dirty = true;
            continue;
        }
        if (!paste && byte == '\t') {
            if (!complete_line(editor, prompt, &line, &length, &capacity,
                               &cursor, previous_tab, &dirty)) goto done;
            previous_tab = true;
            continue;
        }
        previous_tab = false;
        if (byte == 4) {
            if (!length) {
                fputs("\r\n", stdout);
                eof = true;
                break;
            }
            if (cursor < length) {
                size_t end = next_character(line, length, cursor);
                memmove(line + cursor, line + end, length - end + 1);
                length -= end - cursor;
                dirty = true;
            }
            continue;
        }
        if (byte == 8 || byte == 127) {
            if (cursor) {
                size_t start = previous_character(line, cursor);
                memmove(line + start, line + cursor, length - cursor + 1);
                length -= cursor - start;
                cursor = start;
                dirty = true;
            }
            continue;
        }
        if (byte == 1) {
            cursor = 0;
            dirty = true;
            continue;
        }
        if (byte == 5) {
            cursor = length;
            dirty = true;
            continue;
        }
        if (byte == 11) {
            length = cursor;
            line[length] = '\0';
            dirty = true;
            continue;
        }
        if (byte == 21) {
            memmove(line, line + cursor, length - cursor + 1);
            length -= cursor;
            cursor = 0;
            dirty = true;
            continue;
        }
        if (byte == 12) {
            fputs("\x1b[2J\x1b[H", stdout);
            dirty = true;
            continue;
        }
        if (byte == 27) {
            edit_key key = read_escape_sequence(editor);
            if (key == EDIT_KEY_PASTE_END) {
                paste = false;
                pasted_cr = false;
                dirty = true;
                continue;
            }
            if (paste) continue;
            if (key == EDIT_KEY_PASTE_BEGIN) {
                paste = true;
                pasted_cr = false;
                continue;
            }
            const char *replacement = NULL;
            if (key == EDIT_KEY_UP)
                replacement =
                    psvr2_line_editor_previous(editor, line);
            else if (key == EDIT_KEY_DOWN)
                replacement = psvr2_line_editor_next(editor);
            if (replacement &&
                !replace_line(
                    &line, &length, &capacity, &cursor, replacement))
                goto done;
            if (key == EDIT_KEY_LEFT && cursor)
                cursor = previous_character(line, cursor);
            else if (key == EDIT_KEY_RIGHT && cursor < length)
                cursor = next_character(line, length, cursor);
            else if (key == EDIT_KEY_WORD_LEFT)
                cursor = previous_word(line, length, cursor);
            else if (key == EDIT_KEY_WORD_RIGHT)
                cursor = next_word(line, length, cursor);
            else if (key == EDIT_KEY_HOME)
                cursor = 0;
            else if (key == EDIT_KEY_END)
                cursor = length;
            else if (key == EDIT_KEY_DELETE && cursor < length) {
                size_t end = next_character(line, length, cursor);
                memmove(line + cursor, line + end, length - end + 1);
                length -= end - cursor;
            }
            if (key != EDIT_KEY_NONE)
                dirty = true;
            continue;
        }
        if (byte < 0x20 || byte == 0x7f) continue;
        if (!reserve_line(&line, &capacity, length + 1)) goto done;
        memmove(
            line + cursor + 1, line + cursor,
            length - cursor + 1);
        line[cursor++] = (char)byte;
        ++length;
        dirty = true;
    }

done:
    fputs("\x1b[?2004l", stdout);
    fflush(stdout);
    (void)tcsetattr(STDIN_FILENO, TCSANOW, &original);
    if (text_locale) {
        (void)uselocale(previous_locale);
        freelocale(text_locale);
    }
    if (!accepted || eof) {
        free(line);
        return false;
    }
    *line_out = line;
    return true;
}
#endif

bool psvr2_line_editor_read(
    psvr2_line_editor *editor, const char *prompt, char **line_out) {
    if (!editor || !prompt || !line_out) return false;
    *line_out = NULL;
#if !defined(_WIN32)
    bool interactive =
        isatty(STDIN_FILENO) && isatty(STDOUT_FILENO);
    bool read_ok = interactive
        ? read_terminal_line(editor, prompt, line_out)
        : read_stream_line(prompt, line_out);
#else
    bool read_ok = read_stream_line(prompt, line_out);
#endif
    if (!read_ok) return false;
    if (psvr2_line_editor_add_history(editor, *line_out))
        return true;
    free(*line_out);
    *line_out = NULL;
    return false;
}
