#ifndef PSVR2_SHELL_COMPLETION_INTERNAL_H
#define PSVR2_SHELL_COMPLETION_INTERNAL_H

#include "line_editor_internal.h"
#include "psvr2/shell.h"

bool psvr2_shell_complete(
    void *context, const char *line, size_t cursor,
    psvr2_line_completions *out);

/* Append allocated raw command names; the provider escapes and sorts them. */
bool psvr2_shell_complete_commands(
    psvr2_shell *shell, const char *prefix, psvr2_line_completions *out);

#endif
