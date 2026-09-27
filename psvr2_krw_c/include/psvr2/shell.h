#ifndef PSVR2_SHELL_H
#define PSVR2_SHELL_H

#include "psvr2/shellcode.h"

typedef struct {
    psvr2_runtime *runtime;
    psvr2_vfs vfs;
    uint64_t cwd;
    char cwd_name[1024];
    bool read_only;
    bool force_tmp;
    bool noevict;
    bool double_evict;
    bool running;
    int bulk_interface;
    uint8_t bulk_in;
    uint8_t bulk_out;
    uint16_t bulk_packet;
    bool bulk_claimed;
    uint64_t bulk_generation;
    char busybox_path[64];
    char busybox_cwd[1024];
    bool busybox_announced;
    bool busybox_mode;
    struct psvr2_shell_job *jobs;
    unsigned job_sequence;
    uint64_t job_session;
} psvr2_shell;

void psvr2_shell_init(psvr2_shell *shell, psvr2_runtime *runtime,
                      bool read_only, bool force_tmp, bool noevict,
                      bool double_evict);
void psvr2_shell_destroy(psvr2_shell *shell);
int psvr2_shell_run(psvr2_shell *shell);
int psvr2_shell_execute(psvr2_shell *shell, const char *line);

#endif
