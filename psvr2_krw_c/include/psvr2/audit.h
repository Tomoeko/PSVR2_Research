#ifndef PSVR2_AUDIT_H
#define PSVR2_AUDIT_H

#include "psvr2/memory.h"

typedef struct {
    bool exact_memory_layout;
    bool bounded_probe_safe;
    bool injected_execution_safe;
} psvr2_safety_report;

bool psvr2_memory_safety_audit(psvr2_runtime *runtime,
                               psvr2_safety_report *report);

#endif
