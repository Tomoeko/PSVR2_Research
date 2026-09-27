#ifndef PSVR2_SHELLCODE_H
#define PSVR2_SHELLCODE_H

#include "psvr2/memory.h"
#include "psvr2/stage1.h"

bool psvr2_verify_shellcode(psvr2_runtime *runtime, bool fatal);
bool psvr2_inject_str_shellcode(psvr2_runtime *runtime);
bool psvr2_exec(psvr2_runtime *runtime, const char *command,
                bool capture_output, int64_t *retval, psvr2_buffer *output);
bool psvr2_exec_blind(psvr2_runtime *runtime, const char *command);

#endif
