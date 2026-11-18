#ifndef NGAGE_SHIM_CPU_H
#define NGAGE_SHIM_CPU_H
/* Guest memory for the tests: a flat buffer at shim_mem_base. */
typedef struct CPUState { int dummy; } CPUState;
extern CPUState *first_cpu;
extern uint8_t shim_mem[65536];
extern uint32_t shim_mem_base;
static inline int cpu_memory_rw_debug(CPUState *cpu, uint32_t va, void *buf, size_t len, bool w)
{
    (void)cpu;
    if (va < shim_mem_base || va + len > shim_mem_base + sizeof(shim_mem)) {
        return -1;
    }
    if (w) {
        memcpy(shim_mem + (va - shim_mem_base), buf, len);
    } else {
        memcpy(buf, shim_mem + (va - shim_mem_base), len);
    }
    return 0;
}
#endif
