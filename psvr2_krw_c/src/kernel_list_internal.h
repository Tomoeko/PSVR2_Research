#ifndef PSVR2_KERNEL_LIST_INTERNAL_H
#define PSVR2_KERNEL_LIST_INTERNAL_H

#include "psvr2/rw.h"

typedef bool (*psvr2_kernel_list_node_fn)(
    void *opaque, uint64_t list_node);
typedef bool (*psvr2_kernel_list_read_fn)(
    void *opaque, uint64_t address, uint64_t *value);

psvr2_walk_result psvr2_kernel_list_walk_with_reader(
    psvr2_kernel_list_read_fn read_pointer, void *read_opaque,
    uint64_t first_node, uint64_t stop_node,
    bool visit_stop_node, size_t limit,
    psvr2_kernel_list_node_fn fn, void *opaque);

psvr2_walk_result psvr2_kernel_list_walk(
    psvr2_krw *krw, uint64_t first_node, uint64_t stop_node,
    bool visit_stop_node, size_t limit,
    psvr2_kernel_list_node_fn fn, void *opaque);

#endif
