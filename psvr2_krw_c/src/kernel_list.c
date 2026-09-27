#include "kernel_list_internal.h"

#include <stdlib.h>

static size_t pointer_hash(uint64_t value, size_t mask) {
    value ^= value >> 33;
    value *= UINT64_C(0xff51afd7ed558ccd);
    value ^= value >> 33;
    return (size_t)value & mask;
}

static bool visited_insert(
    uint64_t *table, size_t capacity, uint64_t node) {
    size_t mask = capacity - 1;
    size_t slot = pointer_hash(node, mask);
    for (size_t probe = 0; probe < capacity; ++probe) {
        if (table[slot] == node) return false;
        if (table[slot] == 0) {
            table[slot] = node;
            return true;
        }
        slot = (slot + 1) & mask;
    }
    return false;
}

psvr2_walk_result psvr2_kernel_list_walk_with_reader(
    psvr2_kernel_list_read_fn read_pointer, void *read_opaque,
    uint64_t first_node, uint64_t stop_node,
    bool visit_stop_node, size_t limit,
    psvr2_kernel_list_node_fn fn, void *opaque) {
    if (!read_pointer || !first_node || !limit || !fn)
        return PSVR2_WALK_INVALID;
    if (first_node == stop_node && !visit_stop_node)
        return PSVR2_WALK_COMPLETE;

    size_t capacity = 8;
    while (capacity < limit * 2) {
        if (capacity > SIZE_MAX / 2) return PSVR2_WALK_INVALID;
        capacity *= 2;
    }
    uint64_t *visited = calloc(capacity, sizeof(*visited));
    if (!visited) return PSVR2_WALK_INVALID;

    psvr2_walk_result result = PSVR2_WALK_LIMIT;
    uint64_t node = first_node;
    for (size_t count = 0; count < limit; ++count) {
        if (!node || (node == stop_node &&
                      !(visit_stop_node && count == 0))) {
            result = PSVR2_WALK_COMPLETE;
            break;
        }
        if (!visited_insert(visited, capacity, node)) {
            result = node == stop_node
                         ? PSVR2_WALK_COMPLETE : PSVR2_WALK_CYCLE;
            break;
        }
        if (!fn(opaque, node)) {
            result = PSVR2_WALK_CALLBACK_STOPPED;
            break;
        }
        uint64_t next = 0;
        if (!read_pointer(read_opaque, node, &next)) {
            result = PSVR2_WALK_READ_ERROR;
            break;
        }
        node = next;
    }
    free(visited);
    return result;
}

static bool read_krw_pointer(
    void *opaque, uint64_t address, uint64_t *value) {
    uint8_t encoded[8];
    psvr2_krw *krw = opaque;
    if (!psvr2_krw_read(
            krw, address, encoded, sizeof(encoded)))
        return false;
    *value = psvr2_load_le64(encoded);
    return true;
}

psvr2_walk_result psvr2_kernel_list_walk(
    psvr2_krw *krw, uint64_t first_node, uint64_t stop_node,
    bool visit_stop_node, size_t limit,
    psvr2_kernel_list_node_fn fn, void *opaque) {
    if (!krw) return PSVR2_WALK_INVALID;
    return psvr2_kernel_list_walk_with_reader(
        read_krw_pointer, krw, first_node, stop_node,
        visit_stop_node, limit, fn, opaque);
}
