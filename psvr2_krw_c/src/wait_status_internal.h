#ifndef PSVR2_WAIT_STATUS_INTERNAL_H
#define PSVR2_WAIT_STATUS_INTERNAL_H

#include <stdint.h>

/* UMH_WAIT_PROC returns a Linux wait status, not a shell exit code. Decode
 * at the transport boundary exactly once, using target Linux bit fields
 * rather than the host platform's wait macros. Negative kernel errors retain
 * their value; terminated processes use the shell convention 128 + signal.
 * Stopped, continued, and otherwise unexpected statuses remain unchanged. */
static inline int64_t psvr2_remote_exit_status(int64_t status) {
    /* The injected UMH worker stores x0 after a function returning int. A
     * negative return in w0 can therefore arrive zero-extended to 64 bits. */
    if (status >= INT64_C(0x80000000) && status <= INT64_C(0xffffffff))
        status -= INT64_C(0x100000000);
    if (status < 0 || status > INT64_C(0xffff))
        return status;
    if ((status & INT64_C(0xff)) == 0)
        return (status >> 8) & INT64_C(0xff);
    int64_t signal = status & INT64_C(0x7f);
    if ((status & INT64_C(0xff00)) == 0 && signal > 0 && signal < 0x7f)
        return 128 + signal;
    return status;
}

#endif
