/*
 * glserver: the few QEMU interfaces syborg_gl.c (src/qemu-syborg/hw/arm, unchanged) uses, for running the
 * syborg-gl device outside QEMU: as the host GL server of the hosted EKA2 board (src/hosted/glserver,
 * docs/hosted-eka2/graphics.md). Threads, locks and timers are pthreads; QOM is reduced to plain structs;
 * guest memory goes through the server's channel back to the hosted kernel:
 *   - the "TLB" reports every page as MMIO, so syborg_gl.c's user_rw() takes its address-space path;
 *   - cpu_get_address_space() names the user (virtual) space, address_space_memory the physical one;
 *   - address_space_read/write() then become channel requests (glserver.c: gs_mem).
 * The process identity a kick or a BIND uses (the ARM TTBR0 in QEMU) is the hosted process id.
 * Original work for qembian. SPDX-License-Identifier: GPL-2.0-or-later (built with syborg_gl.c)
 */
#ifndef QEMU_SHIM_H
#define QEMU_SHIM_H

#define _GNU_SOURCE
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdarg.h>
#include <string.h>
#include <errno.h>
#include <fcntl.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <glib.h>

typedef uint64_t hwaddr;
#ifndef MIN
#define MIN(a, b) ((a) < (b) ? (a) : (b))
#endif
#ifndef MAX
#define MAX(a, b) ((a) > (b) ? (a) : (b))
#endif
#define QEMU_PACKED __attribute__((packed))
#define G_GNUC_PRINTF_SHIM(a, b) __attribute__((format(printf, a, b)))

/* ---- logging */
#define LOG_GUEST_ERROR 1
#define LOG_UNIMP 2
void qemu_log_mask(int mask, const char *fmt, ...) G_GNUC_PRINTF_SHIM(2, 3);
void warn_report(const char *fmt, ...) G_GNUC_PRINTF_SHIM(1, 2);
void info_report(const char *fmt, ...) G_GNUC_PRINTF_SHIM(1, 2);
void error_report(const char *fmt, ...) G_GNUC_PRINTF_SHIM(1, 2);
typedef struct Error Error;
void error_setg_shim(Error **errp, const char *fmt, ...);
#define error_setg error_setg_shim
#define error_setg_errno(errp, err, ...) error_setg_shim(errp, __VA_ARGS__)
#define cpu_to_le32(x) ((uint32_t)(x))         /* little-endian hosts only (x86-64, arm64) */
#define le32_to_cpu(x) ((uint32_t)(x))

/* ---- atomics */
#define qatomic_read(p) __atomic_load_n(p, __ATOMIC_SEQ_CST)
#define qatomic_set(p, v) __atomic_store_n(p, v, __ATOMIC_SEQ_CST)
#define qatomic_xchg(p, v) __atomic_exchange_n(p, v, __ATOMIC_SEQ_CST)
#define qatomic_inc(p) __atomic_add_fetch(p, 1, __ATOMIC_SEQ_CST)
#define qatomic_dec(p) __atomic_sub_fetch(p, 1, __ATOMIC_SEQ_CST)
#define qatomic_add(p, v) __atomic_add_fetch(p, v, __ATOMIC_SEQ_CST)
#define qatomic_cmpxchg(p, o, n) __sync_val_compare_and_swap(p, o, n)
#define smp_mb() __atomic_thread_fence(__ATOMIC_SEQ_CST)
#define smp_wmb() __atomic_thread_fence(__ATOMIC_RELEASE)
#define smp_rmb() __atomic_thread_fence(__ATOMIC_ACQUIRE)

/* ---- threads */
typedef struct { pthread_mutex_t m; } QemuMutex;
typedef struct { pthread_cond_t c; } QemuCond;
typedef struct { pthread_t t; } QemuThread;
#define QEMU_THREAD_JOINABLE 0
#define QEMU_THREAD_DETACHED 1
static inline void qemu_mutex_init(QemuMutex *m) { pthread_mutex_init(&m->m, NULL); }
static inline void qemu_mutex_lock(QemuMutex *m) { pthread_mutex_lock(&m->m); }
static inline void qemu_mutex_unlock(QemuMutex *m) { pthread_mutex_unlock(&m->m); }
static inline void qemu_cond_init(QemuCond *c) { pthread_cond_init(&c->c, NULL); }
static inline void qemu_cond_wait(QemuCond *c, QemuMutex *m) { pthread_cond_wait(&c->c, &m->m); }
static inline void qemu_cond_signal(QemuCond *c) { pthread_cond_signal(&c->c); }
static inline void qemu_cond_broadcast(QemuCond *c) { pthread_cond_broadcast(&c->c); }
static inline void qemu_thread_create(QemuThread *t, const char *name, void *(*fn)(void *), void *arg, int mode)
{
    (void)name; (void)mode;
    pthread_create(&t->t, NULL, fn, arg);
}
static inline bool qemu_thread_is_self(QemuThread *t) { return pthread_equal(t->t, pthread_self()); }
#define rcu_register_thread() do { } while (0)
#define rcu_unregister_thread() do { } while (0)

/* ---- clocks and timers (fired by glserver's loop between requests: gs_timers) */
#define QEMU_CLOCK_REALTIME 0
#define QEMU_CLOCK_VIRTUAL 1
#define SCALE_MS 1000000
int64_t qemu_clock_get_ms(int clock);
int64_t qemu_clock_get_ns(int clock);
int64_t get_clock(void);                        /* monotonic ns */
typedef struct QEMUTimer { void (*cb)(void *); void *opaque; int64_t deadline_ms; } QEMUTimer;
QEMUTimer *timer_new_ms(int clock, void (*cb)(void *), void *opaque);
void timer_mod(QEMUTimer *t, int64_t deadline_ms);
void timer_del(QEMUTimer *t);

/* ---- QOM, reduced: a device is its state struct */
typedef struct Object { int dummy; } Object;
typedef struct DeviceState { Object obj; } DeviceState;
typedef struct SysBusDevice { DeviceState dev; } SysBusDevice;
typedef struct ObjectClass { int dummy; } ObjectClass;
typedef struct DeviceClass { void (*realize)(DeviceState *, Error **); } DeviceClass;
typedef struct Visitor Visitor;
typedef struct QEnumLookup { const char *const *array; int size; } QEnumLookup;
typedef struct Property { const char *name; } Property;
typedef struct TypeInfo { const char *name, *parent; size_t instance_size;
                          void (*instance_init)(Object *); void (*class_init)(ObjectClass *, const void *); } TypeInfo;
#define OBJECT_DECLARE_SIMPLE_TYPE(T, M) typedef struct T T; static inline T *M(void *o) { return (T *)o; }
#define OBJECT(o) ((Object *)(o))
#define DEVICE_CLASS(oc) ((DeviceClass *)(oc))
#define SYS_BUS_DEVICE(o) ((SysBusDevice *)(o))
#define TYPE_SYS_BUS_DEVICE "sys-bus-device"
#define DEFINE_TYPES(t)
#define DEFINE_PROP_STRING(n, T, f) { n }
#define DEFINE_PROP_BOOL(n, T, f, d) { n }
#define DEFINE_PROP_UINT32(n, T, f, d) { n }
#define DEFINE_PROP_END_OF_LIST() { NULL }
static inline void device_class_set_props(DeviceClass *dc, const Property *p) { (void)dc; (void)p; }
static inline void device_class_set_legacy_reset(DeviceClass *dc, void (*f)(DeviceState *)) { (void)dc; (void)f; }
typedef void ObjectPropertyAccessor(Object *, Visitor *, const char *, void *, Error **);
static inline void object_property_add(Object *o, const char *n, const char *t, ObjectPropertyAccessor *g,
                                       ObjectPropertyAccessor *s, void *r, void *op) { }
static inline void object_property_set_description(Object *o, const char *n, const char *d) { }
static inline void object_property_add_enum(Object *o, const char *n, const char *t, const QEnumLookup *l,
                                            int (*g)(Object *, Error **), void (*s)(Object *, int, Error **)) { }
static inline void object_property_add_str(Object *o, const char *n, char *(*g)(Object *, Error **),
                                           void (*s)(Object *, const char *, Error **)) { }
static inline bool visit_type_uint32(Visitor *v, const char *n, uint32_t *p, Error **e) { return false; }

/* ---- memory: the device's MMIO region (glserver calls the ops directly) */
typedef struct MemTxAttrs { unsigned int user:1, secure:1; } MemTxAttrs;
typedef int MemTxResult;
#define MEMTX_OK 0
#define MEMTX_ERROR 1
#define MEMTXATTRS_UNSPECIFIED ((MemTxAttrs){ 0 })
enum device_endian { DEVICE_LITTLE_ENDIAN };
typedef struct MemoryRegionOps {
    MemTxResult (*read_with_attrs)(void *, hwaddr, uint64_t *, unsigned, MemTxAttrs);
    MemTxResult (*write_with_attrs)(void *, hwaddr, uint64_t, unsigned, MemTxAttrs);
    enum device_endian endianness;
    struct { unsigned min_access_size, max_access_size; } valid;
} MemoryRegionOps;
typedef struct MemoryRegion { const MemoryRegionOps *ops; void *opaque; } MemoryRegion;
static inline void memory_region_init_io(MemoryRegion *mr, Object *o, const MemoryRegionOps *ops, void *opaque,
                                         const char *name, uint64_t size) { mr->ops = ops; mr->opaque = opaque; }
static inline void sysbus_init_mmio(SysBusDevice *d, MemoryRegion *mr) { }

/* ---- guest memory and the "CPU" (glserver.c) */
typedef struct AddressSpace { int virt; } AddressSpace;
extern AddressSpace address_space_memory;               /* physical */
extern AddressSpace gs_user_space;                      /* the kick's process, virtual */
MemTxResult address_space_read(AddressSpace *as, hwaddr a, MemTxAttrs at, void *p, hwaddr n);
MemTxResult address_space_write(AddressSpace *as, hwaddr a, MemTxAttrs at, const void *p, hwaddr n);
typedef struct CPUARMState { uint64_t ttbr0; } CPUARMState;
typedef struct CPUState { CPUARMState env; } CPUState;
extern CPUState *current_cpu;
static inline CPUARMState *cpu_env(CPUState *cs) { return &cs->env; }
static inline int arm_mmu_idx_el(CPUARMState *env, int el) { return 0; }
static inline int arm_to_core_mmu_idx(int i) { return i; }
#define A32_BANKED_CURRENT_REG_GET(env, r) ((env)->r)
typedef struct CPUTLBEntryFull { hwaddr phys_addr; MemTxAttrs attrs; } CPUTLBEntryFull;
#define TLB_INVALID_MASK (1 << 0)
#define TLB_MMIO (1 << 1)
#define MMU_DATA_LOAD 0
#define MMU_DATA_STORE 1
size_t qemu_target_page_size(void);
int probe_access_full_mmu(CPUARMState *env, uint32_t va, int size, int access, int idx, void **host,
                          CPUTLBEntryFull **full);
static inline AddressSpace *cpu_get_address_space(CPUState *cs, int idx) { return &gs_user_space; }
static inline int cpu_asidx_from_attrs(CPUState *cs, MemTxAttrs a) { return 0; }

#endif
