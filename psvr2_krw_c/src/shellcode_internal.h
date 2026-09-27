#ifndef PSVR2_SHELLCODE_INTERNAL_H
#define PSVR2_SHELLCODE_INTERNAL_H

#include "psvr2/shellcode.h"

bool psvr2_runtime_execution_safe(
    psvr2_runtime *runtime, bool explain);
bool psvr2_runtime_workspace_safe(
    psvr2_runtime *runtime, bool explain);

#endif
