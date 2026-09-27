#include "line_editor_internal.h"

#include <stdlib.h>
#include <string.h>

static bool complete_fixture(
    void *context, const char *line, size_t cursor, psvr2_line_completions *out) {
    (void)context;
    fputs("COMPLETE\n", stderr);
    fflush(stderr);
    size_t start = cursor;
    size_t end = cursor;
    while (start && line[start - 1] != ' ')
        --start;
    while (line[end] && line[end] != ' ')
        ++end;
    out->start = start;
    out->end = end;

    const char *candidates[3] = {NULL, NULL, NULL};
    size_t count = 0;
    if (cursor - start == 6 && !memcmp(line + start, "./open", 6)) {
        candidates[count++] = line[end] == ' ' ? "./open_vrhmd" : "./open_vrhmd ";
    } else if (cursor - start == 5 && !memcmp(line + start, "./dir", 5)) {
        candidates[count++] = "./directory/";
    } else if (cursor - start == 4 && !memcmp(line + start, "./al", 4)) {
        candidates[count++] = "./alpha";
        candidates[count++] = "./alpine";
    } else if (cursor - start == 5 && !memcmp(line + start, "./alp", 5)) {
        candidates[count++] = "./alpha";
        candidates[count++] = "./alpine";
    } else if (cursor - start == 5 && !memcmp(line + start, "'./al", 5)) {
        candidates[count++] = "./alpha";
        candidates[count++] = "./alpine";
    } else if (cursor - start == 3 && !memcmp(line + start, "./a", 3)) {
        candidates[count++] = "./a\\ b";
        candidates[count++] = "./a\\?b";
    } else if ((cursor - start == 4 && !memcmp(line + start, "./ca", 4)) ||
               (cursor - start == 5 && !memcmp(line + start, "./caf", 5))) {
        candidates[count++] = "./caf\xc3\xa9";
        candidates[count++] = "./caf\xc3\xaa";
    } else if (cursor == 7 && !memcmp(line, "./My\\ F", 7)) {
        out->start = 0;
        out->end = end;
        candidates[count++] = "./My\\ File ";
    } else if (cursor == 7 && !memcmp(line, "'./My F", 7)) {
        out->start = 0;
        out->end = end;
        candidates[count++] = "'./My File' ";
    }
    if (!count)
        return true;
    out->items = calloc(count, sizeof(*out->items));
    if (!out->items)
        return false;
    for (size_t index = 0; index < count; ++index) {
        out->items[index] = strdup(candidates[index]);
        if (!out->items[index]) {
            for (size_t previous = 0; previous < index; ++previous)
                free(out->items[previous]);
            free(out->items);
            out->items = NULL;
            return false;
        }
    }
    out->count = count;
    return true;
}

int main(int argc, char **argv) {
    unsigned long reads = argc > 1 ? strtoul(argv[1], NULL, 10) : 1;
    const char *prompt = argc > 2 ? argv[2] : "PSVR2> ";
    psvr2_line_editor editor;
    psvr2_line_editor_init(&editor);
    if (argc > 3 && !strcmp(argv[3], "complete"))
        editor.complete = complete_fixture;
    for (unsigned long index = 0; index < reads; ++index) {
        char *line = NULL;
        if (!psvr2_line_editor_read(&editor, prompt, &line)) {
            fputs("EOF\n", stderr);
            break;
        }
        fputs("LINE:", stderr);
        for (const unsigned char *byte = (const unsigned char *)line; *byte; ++byte)
            fprintf(stderr, "%02x", (unsigned)*byte);
        fputc('\n', stderr);
        fflush(stderr);
        free(line);
    }
    psvr2_line_editor_destroy(&editor);
    return 0;
}
