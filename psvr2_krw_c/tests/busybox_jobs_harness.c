/* Exercise actual job supervision locally without USB or Stage1 hardware. */
#include "shell_internal.h"
#include "stage1_internal.h"

#include <assert.h>
#include <errno.h>
#include <fcntl.h>
#include <signal.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <unistd.h>

#include "../src/shell_jobs.c"

static size_t mailbox_calls;
static bool fail_launch_response;
static bool fail_read_response;

/* The target's stock shell rejects group syntax and its BusyBox kill rejects
 * --. Host /bin/sh accepts both, so model those differences explicitly. */
static const char stock_shell_prefix[] =
    "kill() { case \" $* \" in *' -- '*|*' -0 -'*|*' -TERM -'*) "
    "return 2;; esac; command kill \"$@\"; }; ";

bool psvr2_stage1_exec(psvr2_runtime *runtime, const char *command,
                       double timeout, int64_t *result, psvr2_buffer *output) {
    (void)runtime;
    (void)timeout;
    assert(strlen(command) + strlen(PSVR2_STAGE1_COMMAND_PREFIX) <
           PSVR2_STAGE1_COMMAND_CAPACITY);
    ++mailbox_calls;
    int pipes[2];
    assert(pipe(pipes) == 0);
    pid_t child = fork();
    assert(child >= 0);
    if (!child) {
        assert(dup2(pipes[1], STDOUT_FILENO) >= 0);
        assert(dup2(pipes[1], STDERR_FILENO) >= 0);
        close(pipes[0]);
        close(pipes[1]);
        psvr2_buffer script = {0};
        assert(append_text(&script, stock_shell_prefix));
        assert(append_text(&script, command));
        assert(psvr2_buffer_append(&script, "", 1));
        execl("/bin/sh", "sh", "-c", (char *)script.data, (char *)NULL);
        _exit(127);
    }
    close(pipes[1]);
    char bytes[4096];
    ssize_t count;
    while ((count = read(pipes[0], bytes, sizeof(bytes))) != 0) {
        if (count < 0) {
            assert(errno == EINTR);
            continue;
        }
        if (output) assert(psvr2_buffer_append(output, bytes, (size_t)count));
    }
    close(pipes[0]);
    int status;
    while (waitpid(child, &status, 0) < 0) assert(errno == EINTR);
    if (result) *result = WIFEXITED(status) ? WEXITSTATUS(status) :
                        128 + WTERMSIG(status);
    if (output) {
        assert(psvr2_buffer_reserve(output, output->len + 1));
        output->data[output->len] = '\0';
    }
    /* A command can have launched even when its mailbox response is lost. */
    if (fail_launch_response && strstr(command, "setsid") &&
        strstr(command, "/launch")) {
        fail_launch_response = false;
        return false;
    }
    if (fail_read_response && strstr(command, "printf 'D '")) {
        fail_read_response = false;
        return false;
    }
    return true;
}

static int fake_busybox(int argc, char **argv) {
    if (argc >= 2 && !strcmp(argv[1], "ash")) {
        assert(argc >= 3 && strcmp(argv[2], "-c"));
        psvr2_buffer script = {0};
        assert(append_text(&script, stock_shell_prefix));
        /* Force cancellation after the supervisor publishes its PID but
         * before its second cancel check, exercising the early TERM branch. */
        assert(append_text(&script,
            "mv() { command mv \"$@\" || return; "
            "if [ \"${PSVR2_TEST_EARLY_CANCEL-}\" = 1 ] && "
            "[ \"$2\" = \"$d/pid\" ]; then : >\"$d/cancel\"; fi; }; "
            "s=$1; shift; . \"$s\""));
        assert(psvr2_buffer_append(&script, "", 1));
        char **arguments = calloc((size_t)argc + 3, sizeof(*arguments));
        assert(arguments);
        arguments[0] = (char *)"sh";
        arguments[1] = (char *)"-c";
        arguments[2] = (char *)script.data;
        arguments[3] = (char *)"sh";
        for (int i = 2; i < argc; ++i) arguments[i + 2] = argv[i];
        execv("/bin/sh", arguments);
        return 127;
    }
    if (argc >= 2 && !strcmp(argv[1], "kill")) {
        /* Deliberately reject --, exactly as the installed headset applet. */
        if (argc != 4) return 2;
        int signal_number = !strcmp(argv[2], "-0") ? 0 :
                            !strcmp(argv[2], "-TERM") ? SIGTERM : -1;
        char *end = NULL;
        errno = 0;
        long target = strtol(argv[3], &end, 10);
        if (signal_number < 0 || errno || end == argv[3] || *end ||
            target < -INT_MAX || target > INT_MAX) return 2;
        /* Any PID 0/1 reaching this applet is a production validation bug. */
        assert(target > 1 || target < -1);
        return kill((pid_t)target, signal_number) == 0 ? 0 : 1;
    }
    if (argc >= 3 && !strcmp(argv[1], "setsid")) {
        const char *delay = getenv("PSVR2_TEST_SETSID_DELAY_MS");
        if (delay) psvr2_sleep_ms((unsigned)strtoul(delay, NULL, 10));
        assert(setsid() >= 0);
        execv(argv[2], argv + 2);
        return 127;
    }
    return -1;
}

static void finish(psvr2_shell *shell, psvr2_buffer *output, int expected,
                   const char *text) {
    assert(shell->jobs);
    bool complete = false;
    int64_t result = -1;
    double started = psvr2_now();
    do {
        assert(read_job(shell, shell->jobs, true, output, &complete, &result));
        if (!complete) psvr2_sleep_ms(20);
    } while (!complete && psvr2_now() - started < 3.0);
    if (!complete || result != expected) {
        fprintf(stderr, "job expected %d, complete=%d actual=%lld output=[%s]\n",
                expected, complete, (long long)result,
                output->data ? (char *)output->data : "");
        char path[128];
        snprintf(path, sizeof(path), "%s/status", shell->jobs->directory);
        FILE *status_file = fopen(path, "r");
        if (status_file) {
            int status;
            if (fscanf(status_file, "%d", &status) == 1)
                fprintf(stderr, "published remote status=%d\n", status);
            fclose(status_file);
        }
    }
    assert(complete && result == expected);
    if (text) assert(output->data && strstr((char *)output->data, text));
    forget_job(shell, shell->jobs, true);
}

static void write_receipt(const char *directory, const char *name,
                          const char *text) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE *file = fopen(path, "w");
    assert(file && fputs(text, file) >= 0 && fclose(file) == 0);
}

static void read_receipt(const char *directory, const char *name,
                         char *text, size_t capacity) {
    char path[128];
    snprintf(path, sizeof(path), "%s/%s", directory, name);
    FILE *file = fopen(path, "r");
    assert(file && fgets(text, (int)capacity, file) && fclose(file) == 0);
}

static void check_unknown_status(psvr2_shell *shell, psvr2_buffer *output,
                                 int64_t *result) {
    struct psvr2_shell_job *job = shell->jobs;
    assert(job);
    bool complete = false;
    *result = 73;
    assert(!read_job(shell, job, true, output, &complete, result));
    assert(!complete && *result == 73);
    assert(shell->jobs == job && access(job->directory, F_OK) == 0);
}

static void check_host_reentry(const char *busybox, const char *cwd) {
    int receipt[2];
    assert(pipe(receipt) == 0);
    /* Stage1's local shell children must not inherit this host-only pipe. */
    assert(fcntl(receipt[0], F_SETFD, FD_CLOEXEC) == 0);
    assert(fcntl(receipt[1], F_SETFD, FD_CLOEXEC) == 0);
    pid_t first_host = fork();
    assert(first_host >= 0);
    if (!first_host) {
        close(receipt[0]);
        psvr2_runtime runtime = {0};
        psvr2_shell shell = {.runtime = &runtime};
        strcpy(shell.busybox_path, busybox);
        psvr2_buffer output = {0};
        int64_t result = -1;
        assert(psvr2_shell_run_busybox_job(&shell, cwd,
            "read value; printf 'orphan:%s\\n' \"$value\"",
            &output, &result) == PSVR2_SHELL_JOB_RUNNING);
        assert(shell.jobs);
        assert(write(receipt[1], shell.jobs->directory,
                     sizeof(shell.jobs->directory)) ==
               (ssize_t)sizeof(shell.jobs->directory));
        psvr2_shell_jobs_destroy(&shell);
        assert(!shell.jobs);
        psvr2_buffer_free(&output);
        close(receipt[1]);
        _exit(0);
    }
    close(receipt[1]);
    struct psvr2_shell_job orphan = {0};
    size_t received = 0;
    while (received < sizeof(orphan.directory)) {
        ssize_t count = read(receipt[0], orphan.directory + received,
                             sizeof(orphan.directory) - received);
        if (count < 0) {
            assert(errno == EINTR);
            continue;
        }
        assert(count > 0);
        received += (size_t)count;
    }
    close(receipt[0]);
    printf("detached test job: %s\n", orphan.directory);
    fflush(stdout);
    int host_status;
    while (waitpid(first_host, &host_status, 0) < 0) assert(errno == EINTR);
    assert(WIFEXITED(host_status) && WEXITSTATUS(host_status) == 0);

    psvr2_runtime next_runtime = {0};
    psvr2_shell next_shell = {.runtime = &next_runtime};
    strcpy(next_shell.busybox_path, busybox);
    assert(!next_shell.jobs && !next_shell.job_sequence && !next_shell.job_session);
    psvr2_buffer output = {0};
    int64_t result = -1;
    bool complete = false;
    assert(read_job(&next_shell, &orphan, false, &output, &complete, &result));
    assert(!complete);
    assert(psvr2_shell_run_busybox_job(&next_shell, cwd, "echo new-session",
                                      &output, &result) == PSVR2_SHELL_JOB_COMPLETE);
    assert(result == 0 && strstr((char *)output.data, "new-session"));
    assert(!next_shell.jobs);
    assert(read_job(&next_shell, &orphan, false, &output, &complete, &result));
    assert(!complete);

    /* The path is deliberately retained by the test, not imported by the new
     * shell. Host-session job IDs are not a cross-session discovery mechanism. */
    char path[128];
    snprintf(path, sizeof(path), "%s/in", orphan.directory);
    int input = open(path, O_WRONLY | O_NONBLOCK);
    assert(input >= 0);
    assert(write(input, "reentered\n", 10) == 10);
    close(input);
    double started = psvr2_now();
    do {
        assert(read_job(&next_shell, &orphan, true, &output, &complete, &result));
        if (!complete) psvr2_sleep_ms(20);
    } while (!complete && psvr2_now() - started < 3.0);
    assert(complete && result == 0);
    assert(output.data && strstr((char *)output.data, "orphan:reentered"));
    snprintf(path, sizeof(path), "rm -rf %s", orphan.directory);
    assert(control(&next_shell, path, NULL));
    psvr2_buffer_free(&output);
    psvr2_shell_jobs_destroy(&next_shell);
}

int main(int argc, char **argv) {
    int fake = fake_busybox(argc, argv);
    if (fake >= 0) return fake;
    assert(argc == 3);
    psvr2_runtime runtime = {0};
    psvr2_shell shell = {.runtime = &runtime};
    assert(strlen(argv[0]) < sizeof(shell.busybox_path));
    strcpy(shell.busybox_path, argv[0]);
    psvr2_buffer output = {0};
    int64_t result = -1;

    if (!strcmp(argv[1], "exit")) {
        assert(psvr2_shell_run_busybox_job(&shell, argv[2], "exit 1",
                                          &output, &result) == PSVR2_SHELL_JOB_COMPLETE);
        assert(result == 1 && !shell.jobs);
    } else if (!strcmp(argv[1], "cwd")) {
        assert(psvr2_shell_run_busybox_job(&shell, argv[2], "pwd; false",
                                          &output, &result) == PSVR2_SHELL_JOB_COMPLETE);
        assert(result == 1 && strstr((char *)output.data, argv[2]));
        assert(strstr((char *)output.data, "\036PSVR2_CWD="));
        strcpy(shell.busybox_cwd, "/");
        psvr2_buffer command = {0};
        assert(append_text(&command, "cd ") && append_quoted(&command, argv[2]) &&
               append_text(&command, "; false") &&
               psvr2_buffer_append(&command, "", 1));
        assert(psvr2_shell_busybox_mode_execute(&shell, (char *)command.data) == 1);
        if (strcmp(shell.busybox_cwd, argv[2]))
            fprintf(stderr, "cwd expected [%s], actual [%s]\n", argv[2], shell.busybox_cwd);
        assert(!strcmp(shell.busybox_cwd, argv[2]));
        psvr2_buffer_free(&command);
    } else if (!strcmp(argv[1], "chunks")) {
        char command[4096];
        strcpy(command, "printf '%s' \"");
        size_t prefix = strlen(command);
        memset(command + prefix, '\'', 3000);
        strcpy(command + prefix + 3000, "\"");
        size_t before = mailbox_calls;
        assert(psvr2_shell_run_busybox_job(&shell, argv[2], command,
                                          &output, &result) == PSVR2_SHELL_JOB_COMPLETE);
        assert(result == 0 && mailbox_calls - before > 15);
        for (size_t i = 0; i < 3000; ++i) assert(output.data[i] == '\'');
    } else if (!strcmp(argv[1], "concurrent")) {
        assert(psvr2_shell_run_busybox_job(&shell, argv[2], "sleep 1; echo first",
                                          &output, &result) == PSVR2_SHELL_JOB_RUNNING);
        assert(shell.jobs);
        assert(psvr2_shell_run_busybox_job(&shell, argv[2], "echo second",
                                          &output, &result) == PSVR2_SHELL_JOB_COMPLETE);
        assert(result == 0 && strstr((char *)output.data, "second"));
        finish(&shell, &output, 0, "first");
    } else if (!strcmp(argv[1], "reentry")) {
        check_host_reentry(argv[0], argv[2]);
    } else if (!strcmp(argv[1], "fifo")) {
        assert(psvr2_shell_run_busybox_job(&shell, argv[2],
            "read value; printf '<%s>\\n' \"$value\"",
            &output, &result) == PSVR2_SHELL_JOB_RUNNING);
        assert(shell.jobs);
        char path[128];
        snprintf(path, sizeof(path), "%s/in", shell.jobs->directory);
        int descriptor = open(path, O_WRONLY | O_NONBLOCK);
        assert(descriptor >= 0);
        assert(write(descriptor, "hello\n", 6) == 6);
        close(descriptor);
        finish(&shell, &output, 0, "<hello>");
    } else if (!strcmp(argv[1], "stop")) {
        assert(psvr2_shell_run_busybox_job(&shell, argv[2],
            "sleep 30 & echo $! >child; wait",
            &output, &result) == PSVR2_SHELL_JOB_RUNNING);
        char child_path[1024];
        snprintf(child_path, sizeof(child_path), "%s/child", argv[2]);
        FILE *child_file = fopen(child_path, "r");
        assert(child_file);
        long child_pid = 0;
        assert(fscanf(child_file, "%ld", &child_pid) == 1 && child_pid > 1);
        assert(fclose(child_file) == 0);
        assert(kill((pid_t)child_pid, 0) == 0);
        assert(shell.jobs && signal_job(&shell, shell.jobs));
        finish(&shell, &output, 143, NULL);
        double started = psvr2_now();
        while (kill((pid_t)child_pid, 0) == 0 && psvr2_now() - started < 2.0)
            psvr2_sleep_ms(20);
        assert(kill((pid_t)child_pid, 0) < 0 && errno == ESRCH);
        assert(unlink(child_path) == 0);
    } else if (!strcmp(argv[1], "startup")) {
        assert(setenv("PSVR2_TEST_SETSID_DELAY_MS", "500", 1) == 0);
        assert(psvr2_shell_run_busybox_job(&shell, argv[2], "sleep 30",
                                          &output, &result) == PSVR2_SHELL_JOB_RUNNING);
        assert(shell.jobs);
        bool signalled = signal_job(&shell, shell.jobs);
        /* Cleanup even when this assertion exposes a startup race. */
        unsetenv("PSVR2_TEST_SETSID_DELAY_MS");
        if (!signalled) {
            psvr2_sleep_ms(600);
            assert(signal_job(&shell, shell.jobs));
            finish(&shell, &output, 143, NULL);
        }
        assert(signalled);
        if (shell.jobs) finish(&shell, &output, 143, NULL);
    } else if (!strcmp(argv[1], "cleanup")) {
        assert(psvr2_shell_run_busybox_job(&shell, argv[2],
            "trap 'sleep 0.5; echo cleaned; exit 7' TERM; while :; do sleep 1; done",
            &output, &result) == PSVR2_SHELL_JOB_RUNNING);
        assert(shell.jobs && signal_job(&shell, shell.jobs));
        finish(&shell, &output, 7, "cleaned");
    } else if (!strcmp(argv[1], "early-cancel")) {
        assert(setenv("PSVR2_TEST_EARLY_CANCEL", "1", 1) == 0);
        int state = psvr2_shell_run_busybox_job(&shell, argv[2], "sleep 30",
                                               &output, &result);
        unsetenv("PSVR2_TEST_EARLY_CANCEL");
        assert(state == PSVR2_SHELL_JOB_COMPLETE || state == PSVR2_SHELL_JOB_RUNNING);
        if (state == PSVR2_SHELL_JOB_RUNNING) finish(&shell, &output, 143, NULL);
        else assert(result == 143 && !shell.jobs);
    } else if (!strcmp(argv[1], "failure")) {
        fail_launch_response = true;
        assert(psvr2_shell_run_busybox_job(&shell, argv[2], "sleep 30",
                                          &output, &result) == PSVR2_SHELL_JOB_ERROR);
        assert(shell.jobs && access(shell.jobs->directory, F_OK) == 0);
        assert(signal_job(&shell, shell.jobs));
        finish(&shell, &output, 143, NULL);
    } else if (!strcmp(argv[1], "read-failure")) {
        fail_read_response = true;
        assert(psvr2_shell_run_busybox_job(&shell, argv[2], "sleep 30",
                                          &output, &result) == PSVR2_SHELL_JOB_ERROR);
        assert(shell.jobs && access(shell.jobs->directory, F_OK) == 0);
        /* A later healthy probe still sees the same live process group. */
        bool complete = false;
        assert(read_job(&shell, shell.jobs, false, &output, &complete, &result));
        assert(!complete);
        assert(signal_job(&shell, shell.jobs));
        finish(&shell, &output, 143, NULL);
    } else if (!strcmp(argv[1], "missing-status")) {
        assert(psvr2_shell_run_busybox_job(&shell, argv[2], "sleep 30",
                                          &output, &result) == PSVR2_SHELL_JOB_RUNNING);
        struct psvr2_shell_job *job = shell.jobs;
        char pid[32];
        read_receipt(job->directory, "pid", pid, sizeof(pid));
        assert(kill(-INT_MAX, 0) < 0 && errno == ESRCH);
        write_receipt(job->directory, "pid", "2147483647\n");
        check_unknown_status(&shell, &output, &result);
        char *jobs[] = {(char *)"jobs"};
        char *joblog[] = {(char *)"joblog", (char *)"1"};
        assert(psvr2_shell_jobs_command(&shell, 1, jobs) == 1);
        assert(psvr2_shell_joblog_command(&shell, 2, joblog) == 1);
        assert(shell.jobs == job && access(job->directory, F_OK) == 0);
        write_receipt(job->directory, "pid", pid);
        assert(signal_job(&shell, job));
        finish(&shell, &output, 143, NULL);
    } else if (!strcmp(argv[1], "invalid-pid")) {
        assert(psvr2_shell_run_busybox_job(&shell, argv[2], "sleep 30",
                                          &output, &result) == PSVR2_SHELL_JOB_RUNNING);
        struct psvr2_shell_job *job = shell.jobs;
        char pid[32], launch[32];
        read_receipt(job->directory, "pid", pid, sizeof(pid));
        read_receipt(job->directory, "launch", launch, sizeof(launch));
        const char *invalid[] = {"0\n", "1\n", "0001\n", "-1\n", "abc\n", "\n",
                                "999999999999999999999999999999999999999999\n"};
        for (size_t i = 0; i < PSVR2_ARRAY_LEN(invalid); ++i) {
            write_receipt(job->directory, "pid", invalid[i]);
            check_unknown_status(&shell, &output, &result);
            bool signalled = signal_job(&shell, job);
            write_receipt(job->directory, "pid", pid);
            assert(!signalled);
        }
        char path[128], saved[128];
        snprintf(path, sizeof(path), "%s/pid", job->directory);
        snprintf(saved, sizeof(saved), "%s/pid.saved", job->directory);
        assert(rename(path, saved) == 0);
        for (size_t i = 0; i < PSVR2_ARRAY_LEN(invalid); ++i) {
            write_receipt(job->directory, "launch", invalid[i]);
            check_unknown_status(&shell, &output, &result);
        }
        write_receipt(job->directory, "launch", launch);
        assert(rename(saved, path) == 0);
        assert(signal_job(&shell, job));
        finish(&shell, &output, 143, NULL);
    } else if (!strcmp(argv[1], "malformed")) {
        const char *invalid[] = {"D -1\n", "D 256\n", "D 1x\n", "R bogus\n", "D 1"};
        for (size_t i = 0; i < PSVR2_ARRAY_LEN(invalid); ++i) {
            assert(psvr2_buffer_append(&output, invalid[i], strlen(invalid[i]) + 1));
            --output.len;
            bool complete = false;
            assert(!psvr2_shell_parse_job_output(&output, &complete, &result));
            psvr2_buffer_free(&output);
        }
    } else assert(0 && "unknown test");
    psvr2_buffer_free(&output);
    psvr2_shell_jobs_destroy(&shell);
    return 0;
}
