#ifndef PSVR2_SHELL_INTERNAL_H
#define PSVR2_SHELL_INTERNAL_H

#include "psvr2/shell.h"

#include <stdio.h>

typedef int (*psvr2_shell_command_fn)(psvr2_shell *, int, char **);

typedef struct {
    const char *name;
    psvr2_shell_command_fn function;
    const char *help;
    bool writes;
} psvr2_shell_command;

typedef struct {
    const psvr2_shell_command *commands;
    size_t count;
} psvr2_shell_command_group;

extern const psvr2_shell_command psvr2_shell_vfs_commands[];
extern const size_t psvr2_shell_vfs_command_count;
extern const psvr2_shell_command psvr2_shell_memory_commands[];
extern const size_t psvr2_shell_memory_command_count;
extern const psvr2_shell_command psvr2_shell_stage1_commands[];
extern const size_t psvr2_shell_stage1_command_count;
extern const psvr2_shell_command psvr2_shell_transfer_commands[];
extern const size_t psvr2_shell_transfer_command_count;
extern const psvr2_shell_command psvr2_shell_emmc_commands[];
extern const size_t psvr2_shell_emmc_command_count;
extern const psvr2_shell_command psvr2_shell_persistence_commands[];
extern const size_t psvr2_shell_persistence_command_count;

int psvr2_shell_fast_download(
    psvr2_shell *shell, int argc, char **argv);
int psvr2_shell_fast_upload(
    psvr2_shell *shell, int argc, char **argv);
uint64_t psvr2_shell_stage1_send_size(const char *status);
bool psvr2_shell_sync_bulk_generation(psvr2_shell *shell);
bool psvr2_shell_find_bulk_endpoints(psvr2_shell *shell);
bool psvr2_shell_stop_and_drain_bulk_in(psvr2_shell *shell);
FILE *psvr2_shell_open_output_exclusive(const char *path);
bool psvr2_shell_finalize_output_exclusive(const char *partial,
                                           const char *destination);
bool psvr2_shell_busybox_available(psvr2_shell *shell);
void psvr2_shell_consume_busybox_cwd(psvr2_shell *shell, psvr2_buffer *output);
int psvr2_shell_busybox_execute(
    psvr2_shell *shell, const char *command);
int psvr2_shell_busybox_mode_execute(
    psvr2_shell *shell, const char *command);
bool psvr2_shell_build_busybox_command(
    const char *path, const char *command, psvr2_buffer *out);
bool psvr2_shell_build_busybox_mode_command(
    const char *path, const char *cwd,
    const char *command, psvr2_buffer *out);
bool psvr2_shell_build_serial_command(
    const char *directory, const char *busybox,
    bool reset, bool noevict, bool double_evict, bool reload,
    psvr2_buffer *out);
int psvr2_shell_stage1_execute_line(
    psvr2_shell *shell, const char *command);

enum psvr2_shell_job_result {
    PSVR2_SHELL_JOB_ERROR = -1,
    PSVR2_SHELL_JOB_COMPLETE = 0,
    PSVR2_SHELL_JOB_RUNNING = 1
};
int psvr2_shell_run_busybox_job(
    psvr2_shell *shell, const char *cwd, const char *command,
    psvr2_buffer *output, int64_t *result);
void psvr2_shell_jobs_destroy(psvr2_shell *shell);
int psvr2_shell_jobs_command(psvr2_shell *shell, int argc, char **argv);
int psvr2_shell_joblog_command(psvr2_shell *shell, int argc, char **argv);
int psvr2_shell_jobstop_command(psvr2_shell *shell, int argc, char **argv);
bool psvr2_shell_parse_job_output(
    psvr2_buffer *output, bool *complete, int64_t *result);

#endif
