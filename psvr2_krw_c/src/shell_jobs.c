/* Stage1 is a serialized control mailbox, not an interactive process console.
 * Run BusyBox work in supervised process groups so a persistent command never
 * occupies that mailbox. Its stdin FIFO retains a writer for EOF-sensitive apps.
 */
#include "shell_internal.h"
#include "stage1_internal.h"

#include <errno.h>
#include <limits.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#ifndef PSVR2_SHELL_JOB_WAIT_SECONDS
#define PSVR2_SHELL_JOB_WAIT_SECONDS 15.0
#endif

struct psvr2_shell_job {
    struct psvr2_shell_job *next;
    unsigned id;
    char directory[96];
    char *command;
};

static volatile sig_atomic_t interrupted;
static void interrupt_wait(int signal_number) {
    (void)signal_number;
    interrupted = 1;
}

static bool append_text(psvr2_buffer *buffer, const char *text) {
    return psvr2_buffer_append(buffer, text, strlen(text));
}

static bool append_quoted(psvr2_buffer *buffer, const char *text) {
    if (!append_text(buffer, "'")) return false;
    for (; *text; ++text) {
        if (*text == '\'') {
            if (!append_text(buffer, "'\\''")) return false;
        } else if (!psvr2_buffer_append(buffer, text, 1)) return false;
    }
    return append_text(buffer, "'");
}

static bool control(psvr2_shell *shell, const char *command,
                    psvr2_buffer *output) {
    int64_t result = -1;
    /* Keep each control operation within Stage1's serialized mailbox. */
    return strlen(command) + strlen(PSVR2_STAGE1_COMMAND_PREFIX) <
                   PSVR2_STAGE1_COMMAND_CAPACITY &&
         psvr2_stage1_exec(shell->runtime, command,
                           5.0, &result, output) && result == 0;
}

/* Script chunks avoid imposing the mailbox's 511-byte limit on a user's line.
 * Shell quoting expands literal quote bytes; size each actual
 * invocation before submitting it rather than estimating a fixed chunk size.
 */
static bool write_script(psvr2_shell *shell, const char *directory, const char *name,
                         const char *script) {
    size_t remaining = strlen(script);
    while (remaining) {
        size_t chunk = remaining < 256 ? remaining : 256;
        bool written = false;
        while (chunk) {
            char text[257];
            memcpy(text, script, chunk);
            text[chunk] = '\0';
            psvr2_buffer command = {0};
            char suffix[128];
            snprintf(suffix, sizeof(suffix), " >>%s/%s", directory, name);
            const char end = '\0';
            bool built = append_text(&command, "printf %s ") &&
                append_quoted(&command, text) && append_text(&command, suffix) &&
                psvr2_buffer_append(&command, &end, 1);
            bool fits = built && command.len +
                strlen(PSVR2_STAGE1_COMMAND_PREFIX) <= PSVR2_STAGE1_COMMAND_CAPACITY;
            if (fits) written = control(shell, (char *)command.data, NULL);
            psvr2_buffer_free(&command);
            if (fits) break;
            chunk /= 2;
        }
        if (!written) return false;
        script += chunk;
        remaining -= chunk;
    }
    return true;
}

static bool start_job(psvr2_shell *shell, struct psvr2_shell_job *job,
                      const char *cwd) {
    char command[512];
    snprintf(command, sizeof(command),
        "umask 077; d=%s; mkdir \"$d\" && mkfifo \"$d/in\" && : >\"$d/out\"",
        job->directory);
    if (!control(shell, command, NULL)) return false;

    psvr2_buffer script = {0};
    const char end = '\0';
    bool ok = append_text(&script, "cd ") && append_quoted(&script, cwd) &&
        append_text(&script, " || exit 125\n") && append_text(&script, job->command) &&
        append_text(&script,
            "\n_psvr2_status=$?\nprintf '\\036PSVR2_CWD=%s\\036' \"$PWD\"\n"
            "exit \"$_psvr2_status\"\n") &&
        psvr2_buffer_append(&script, &end, 1) &&
        write_script(shell, job->directory, "cmd", (char *)script.data);
    psvr2_buffer_free(&script);
    if (!ok) return false;

    /* Publish readiness only after the command child exists. A cancel file
     * closes the startup race: an early stop is honored even before setsid
     * creates the group or writes its PID. The supervisor survives SIGTERM
     * long enough to publish the child's status atomically. */
    static const char runner[] =
        "d=$1; trap : TERM\n"
        "if [ -f \"$d/cancel\" ]; then r=143; else\n"
        "  \"$2\" ash \"$d/cmd\" <&3 & c=$!\n"
        "  echo $$ >\"$d/p\"; mv \"$d/p\" \"$d/pid\"\n"
        "  if [ -f \"$d/cancel\" ]; then \"$2\" kill -TERM -\"$$\"; fi\n"
        "  while :; do wait \"$c\"; r=$?; "
        "\"$2\" kill -0 \"$c\" 2>/dev/null || break; done\nfi\n"
        "echo \"$r\" >\"$d/end\"; mv \"$d/end\" \"$d/status\"\n";
    if (!write_script(shell, job->directory, "run", runner)) return false;

    /* setsid's PID is also the process-group ID in a noninteractive ash.
     * A separate supervisor records even an explicit exit/exec in cmd.
     * Open and redirect the FIFO before forking: no blocked mailbox, no EOF,
     * and no background output left attached to Stage1's capture file.
     */
    snprintf(command, sizeof(command),
        "d=%s; b=%s; exec 3<>\"$d/in\"; "
        "\"$b\" setsid \"$b\" ash \"$d/run\" "
        "\"$d\" \"$b\" <&3 >\"$d/out\" 2>&1 & echo $! >\"$d/launch\"",
        job->directory, shell->busybox_path);
    return control(shell, command, NULL);
}

bool psvr2_shell_parse_job_output(psvr2_buffer *output, bool *complete,
                                  int64_t *result) {
    if (!output || !output->data || !output->len || !complete || !result)
        return false;
    const uint8_t *newline = memchr(output->data, '\n', output->len);
    if (!newline) return false;
    size_t length = (size_t)(newline - output->data);
    if (length == 1 && output->data[0] == 'R') {
        *complete = false;
        *result = -1;
    } else if (length >= 3 && length < 16 &&
               output->data[0] == 'D' && output->data[1] == ' ') {
        char text[16];
        memcpy(text, output->data + 2, length - 2);
        text[length - 2] = '\0';
        char *end = NULL;
        errno = 0;
        long number = strtol(text, &end, 10);
        if (errno || end == text || *end || number < 0 || number > 255)
            return false;
        *complete = true;
        *result = number;
    } else return false;
    ++length;
    memmove(output->data, output->data + length, output->len - length);
    output->len -= length;
    output->data[output->len] = '\0';
    return true;
}

static bool read_job(psvr2_shell *shell, struct psvr2_shell_job *job,
                     bool include_output, psvr2_buffer *output,
                     bool *complete, int64_t *result) {
    char command[512];
    snprintf(command, sizeof(command),
        "d=%s; b=%s; s() { [ -f \"$d/status\" ] || exit 125; "
        "printf 'D '; cat \"$d/status\" || exit 125; }; "
        "if [ -f \"$d/status\" ]; then s; "
        "else g=; p=$(cat \"$d/pid\" 2>/dev/null) && g=- "
        "|| p=$(cat \"$d/launch\"); "
        "case $p in ''|*[!0-9]*) exit 125;; esac; "
        "[ \"$p\" -gt 1 ] || exit 125; "
        "\"$b\" kill -0 \"$g$p\" 2>/dev/null && echo R || s; fi%s",
        job->directory, shell->busybox_path,
        include_output ? "; tail -c 64000 \"$d/out\"" : "");
    /* Only an atomically published status proves completion. A missing
     * status, failed probe, or lost response must retain the job and log. The
     * headset's BusyBox kill accepts negative groups but does not accept --. */
    psvr2_buffer_free(output);
    return control(shell, command, output) &&
        psvr2_shell_parse_job_output(output, complete, result);
}

static void forget_job(psvr2_shell *shell, struct psvr2_shell_job *job,
                       bool remove_remote) {
    if (remove_remote) {
        char command[128];
        snprintf(command, sizeof(command), "rm -rf %s", job->directory);
        (void)control(shell, command, NULL);
    }
    struct psvr2_shell_job **link = &shell->jobs;
    while (*link && *link != job) link = &(*link)->next;
    if (*link) *link = job->next;
    free(job->command);
    free(job);
}

static bool signal_job(psvr2_shell *shell, struct psvr2_shell_job *job) {
    char command[512];
    snprintf(command, sizeof(command),
        "d=%s; b=%s; : >\"$d/cancel\"; "
        "if [ ! -f \"$d/status\" ] && [ -f \"$d/pid\" ]; then "
        "p=$(cat \"$d/pid\"); case $p in ''|*[!0-9]*) exit 125;; esac; "
        "[ \"$p\" -gt 1 ] || exit 125; "
        "\"$b\" kill -TERM -\"$p\" || [ -f \"$d/status\" ]; fi",
        job->directory, shell->busybox_path);
    return control(shell, command, NULL);
}

int psvr2_shell_run_busybox_job(psvr2_shell *shell, const char *cwd,
                               const char *command, psvr2_buffer *output,
                               int64_t *result) {
    if (!shell || !cwd || cwd[0] != '/' || !command || !*command ||
        !output || !result || shell->job_sequence == UINT_MAX) return -1;
    struct psvr2_shell_job *job = calloc(1, sizeof(*job));
    if (!job) return -1;
    job->command = strdup(command);
    if (!job->command) { free(job); return -1; }
    job->id = ++shell->job_sequence;
    if (!shell->job_session)
        shell->job_session = (uint64_t)(psvr2_now() * 1000000.0);
    snprintf(job->directory, sizeof(job->directory),
        "/tmp/.psvr2-job-%ld-%llu-%u", (long)getpid(),
        (unsigned long long)shell->job_session, job->id);
    job->next = shell->jobs;
    shell->jobs = job;
    if (!start_job(shell, job, cwd)) {
        fprintf(stderr, "[-] Could not launch BusyBox job %u (%s).\n",
                job->id, job->directory);
        /* A transport failure during launch is ambiguous: never delete the
         * directory of a process that may have started. Retain it for jobs.
         */
        return PSVR2_SHELL_JOB_ERROR;
    }

    interrupted = 0;
    void (*old_handler)(int) = signal(SIGINT, interrupt_wait);
    double start = psvr2_now();
    bool complete = false, readable = true;
    while (psvr2_now() - start < PSVR2_SHELL_JOB_WAIT_SECONDS) {
        if (interrupted) {
            (void)signal_job(shell, job);
            break;
        }
        if (!read_job(shell, job, false, output, &complete, result)) {
            readable = false;
            break;
        }
        if (complete) break;
        psvr2_sleep_ms(100);
    }
    if (old_handler != SIG_ERR) (void)signal(SIGINT, old_handler);
    if (readable)
        readable = read_job(shell, job, true, output, &complete, result);
    if (readable && complete) {
        forget_job(shell, job, true);
        return PSVR2_SHELL_JOB_COMPLETE;
    }
    fprintf(readable ? stdout : stderr,
        "[%s job %u] %s\n  log: %s/out\n"
        "  krw jobs | krw joblog %u | krw jobstop %u\n",
        readable ? (interrupted ? "stopping" : "running") : "status unavailable for",
        job->id, command, job->directory, job->id, job->id);
    return readable ? PSVR2_SHELL_JOB_RUNNING : PSVR2_SHELL_JOB_ERROR;
}

void psvr2_shell_jobs_destroy(psvr2_shell *shell) {
    if (!shell) return;
    while (shell->jobs) forget_job(shell, shell->jobs, false);
}

static struct psvr2_shell_job *find_job(psvr2_shell *shell, const char *text) {
    char *end = NULL;
    errno = 0;
    unsigned long id = strtoul(text, &end, 10);
    if (errno || end == text || *end || !id || id > UINT_MAX) return NULL;
    for (struct psvr2_shell_job *job = shell->jobs; job; job = job->next)
        if (job->id == id) return job;
    return NULL;
}

int psvr2_shell_jobs_command(psvr2_shell *shell, int argc, char **argv) {
    (void)argv;
    if (argc != 1) return 1;
    if (!shell->jobs) { puts("No retained BusyBox jobs."); return 0; }
    int status = 0;
    for (struct psvr2_shell_job *job = shell->jobs; job; job = job->next) {
        psvr2_buffer output = {0};
        int64_t result = -1;
        bool complete = false;
        bool ok = read_job(shell, job, false, &output, &complete, &result);
        printf("[job %u %s", job->id, ok ? (complete ? "exit " : "running") : "unknown");
        if (ok && complete) printf("%lld", (long long)result);
        printf("] %s\n  log: %s/out\n", job->command, job->directory);
        psvr2_buffer_free(&output);
        if (!ok) status = 1;
    }
    return status;
}

int psvr2_shell_joblog_command(psvr2_shell *shell, int argc, char **argv) {
    struct psvr2_shell_job *job = argc == 2 ? find_job(shell, argv[1]) : NULL;
    if (!job) { fputs("Usage: joblog <known-job-id>\n", stderr); return 1; }
    psvr2_buffer output = {0};
    int64_t result;
    bool complete;
    bool ok = read_job(shell, job, true, &output, &complete, &result);
    if (ok) {
        psvr2_shell_consume_busybox_cwd(NULL, &output);
        if (output.len) fwrite(output.data, 1, output.len, stdout);
    }
    psvr2_buffer_free(&output);
    return ok ? 0 : 1;
}

int psvr2_shell_jobstop_command(psvr2_shell *shell, int argc, char **argv) {
    struct psvr2_shell_job *job = argc == 2 ? find_job(shell, argv[1]) : NULL;
    if (!job) { fputs("Usage: jobstop <known-job-id>\n", stderr); return 1; }
    if (!signal_job(shell, job)) return 1;
    printf("[job %u] SIGTERM requested for its process group; use krw jobs to check cleanup.\n", job->id);
    return 0;
}
