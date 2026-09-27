#ifndef PSVR2_LINE_EDITOR_INTERNAL_H
#define PSVR2_LINE_EDITOR_INTERNAL_H

#include "psvr2/common.h"

#define PSVR2_LINE_HISTORY_LIMIT 100U

typedef struct {
    size_t start;
    size_t end;
    char **items;
    size_t count;
} psvr2_line_completions;

/* Replacements cover [start, end); every string and items are caller-owned. */
typedef bool (*psvr2_line_complete_fn)(
    void *context, const char *line, size_t cursor,
    psvr2_line_completions *out);

typedef struct {
    char *entries[PSVR2_LINE_HISTORY_LIMIT];
    size_t count;
    size_t position;
    char *draft;
    /* Keep bytes read ahead from a terminal across successive prompts. */
    unsigned char input[4096];
    size_t input_position;
    size_t input_length;
    psvr2_line_complete_fn complete;
    void *complete_context;
} psvr2_line_editor;

void psvr2_line_editor_init(psvr2_line_editor *editor);
void psvr2_line_editor_destroy(psvr2_line_editor *editor);
void psvr2_line_completions_destroy(psvr2_line_completions *completions);
bool psvr2_line_editor_add_history(
    psvr2_line_editor *editor, const char *line);
const char *psvr2_line_editor_previous(
    psvr2_line_editor *editor, const char *current);
const char *psvr2_line_editor_next(psvr2_line_editor *editor);
bool psvr2_line_editor_read(
    psvr2_line_editor *editor, const char *prompt, char **line_out);

#endif
