/* kill.c — Minimal kill tool using raw syscalls (no libc) */

static int my_atoi(const char *s) {
    int n = 0;
    while (*s >= '0' && *s <= '9')
        n = n * 10 + (*s++ - '0');
    return n;
}

static long sys_kill(int pid, int sig) {
    register long x0 __asm__("x0") = pid;
    register long x1 __asm__("x1") = sig;
    register long x8 __asm__("x8") = 129; /* __NR_kill on aarch64 */
    __asm__ volatile("svc #0" : "+r"(x0) : "r"(x1), "r"(x8) : "memory");
    return x0;
}

static void sys_exit(int code) {
    register long x0 __asm__("x0") = code;
    register long x8 __asm__("x8") = 93; /* __NR_exit */
    __asm__ volatile("svc #0" : : "r"(x0), "r"(x8));
    __builtin_unreachable();
}

void _start(void) {
    /* AArch64 ELF: sp points to argc, then argv[] on stack */
    unsigned long sp;
    __asm__ volatile("mov %0, sp" : "=r"(sp));
    long *stack = (long *)sp;
    int argc = (int)stack[0];
    char **argv = (char **)(stack + 1);

    if (argc > 1) {
        sys_kill(my_atoi(argv[1]), 9);
    }
    sys_exit(0);
}
