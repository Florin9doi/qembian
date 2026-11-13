/*
 * glserver: the syborg-gl device (src/qemu-syborg/hw/arm/syborg_gl.c, compiled here unchanged against the
 * QEMU shim in shim/) as a host process for the hosted EKA2 board. The hosted kernel's register accesses
 * arrive over the channel (include/board/syborg_glchan.h); guest memory the device needs while it runs
 * (command buffers, layers, read-back areas, the composed frame) goes back over the same channel. GL, EGL and
 * OpenVG run here, on the host's GPU (Mesa, surfaceless EGL), exactly as inside QEMU.
 *
 *   glserver --chan FILE [--identity FILE] [--egl-configs FILE] [--gpu-memory-mb N] [--msaa N] [--quiet]
 *
 * Original work for qembian. SPDX-License-Identifier: GPL-2.0-or-later (it contains syborg_gl.c)
 */
#include "qemu_shim.h"
#include <sys/mman.h>
#include <sys/stat.h>
#include <signal.h>
#include "syborg_glchan.h"

/* ---- the shim's state, before the device source sees it */
AddressSpace address_space_memory = { 0 };
AddressSpace gs_user_space = { 1 };
static CPUState gs_cpu;
CPUState *current_cpu = &gs_cpu;

#include "syborg_gl.c"

static volatile uint32_t *Chan;          /* the channel (words) */
static int Quiet;
static uint64_t StCallbacks, StCbBytes, StRequests, StWlog, StPfHits, StBusyNs, StCbWaitNs, StLatNs;
static int Stat;
static unsigned FailLogs, CbFailLogs;

#define W(i) Chan[i]
static inline uint32_t ld(int i) { return __atomic_load_n(&Chan[i], __ATOMIC_ACQUIRE); }
static inline void st(int i, uint32_t v) { __atomic_store_n(&Chan[i], v, __ATOMIC_RELEASE); }

static int64_t now_ns(void)
{
    struct timespec t;
    clock_gettime(CLOCK_MONOTONIC, &t);
    return (int64_t)t.tv_sec * 1000000000 + t.tv_nsec;
}

/* --vsync-hz N (default 60, 0: off): a request that ran an EGL window swap answers at the next vsync */
static int VsyncHz = 60;
extern uint32_t syborg_gl_presents;
static uint32_t SeenPresents;
static uint64_t StPaced;

/* poll a word until it equals a value: spin a while (latency under TCG), then sleep in small steps */
static int wait_eq(int i, uint32_t v, int64_t timeout_ns)
{
    int64_t t0 = now_ns();
    for (unsigned n = 0;; n++) {
        if (ld(i) == v) {
            return 0;
        }
        if (n < 4000) {
            __builtin_ia32_pause();
            continue;
        }
        struct timespec d = { 0, 20000 };
        nanosleep(&d, NULL);
        if (timeout_ns && now_ns() - t0 > timeout_ns) {
            return -1;
        }
    }
}

/* ---- guest memory over the channel */
/* v3: the request's prefetched virtual memory and its write log (syborg_glchan.h) */
static uint32_t PfN, WlogN, WlogBytes;
static int InRequest;                   /* timer work between requests uses plain callbacks */

static uint8_t *pf_find(uint32_t addr, size_t n)
{
    for (uint32_t i = 0; i < PfN; i++) {
        uint32_t va = ld(GLCHAN_W_PF + 3 * i), len = ld(GLCHAN_W_PF + 3 * i + 1), off = ld(GLCHAN_W_PF + 3 * i + 2);
        if (addr >= va && addr - va <= len && n <= len - (addr - va)) {
            return (uint8_t *)(uintptr_t)Chan + GLCHAN_PF + off + (addr - va);
        }
    }
    return NULL;
}

/* a virtual write into the log (and into prefetched copies it overlaps); 0 when it does not fit */
static int wlog_add(uint32_t addr, const void *p, size_t n)
{
    size_t need = 8 + ((n + 3) & ~(size_t)3);
    if (WlogBytes + need > GLCHAN_WLOG_MAX) {
        return 0;
    }
    uint8_t *e = (uint8_t *)(uintptr_t)Chan + GLCHAN_WLOG + WlogBytes;
    ((uint32_t *)e)[0] = addr;
    ((uint32_t *)e)[1] = (uint32_t)n;
    memcpy(e + 8, p, n);
    WlogBytes += need;
    for (uint32_t i = 0; i < PfN; i++) {
        uint32_t va = ld(GLCHAN_W_PF + 3 * i), len = ld(GLCHAN_W_PF + 3 * i + 1), off = ld(GLCHAN_W_PF + 3 * i + 2);
        uint32_t lo = MAX(va, addr), hi = MIN((uint64_t)va + len, (uint64_t)addr + n);
        if (lo < hi) {
            memcpy((uint8_t *)(uintptr_t)Chan + GLCHAN_PF + off + (lo - va), (const uint8_t *)p + (lo - addr), hi - lo);
        }
    }
    st(GLCHAN_W_WLOG_BYTES, WlogBytes);
    st(GLCHAN_W_WLOG_N, ++WlogN);           /* the board applies entries up to here before any callback */
    StWlog++;
    return 1;
}

static int gs_mem(int op, uint32_t addr, void *p, size_t n)
{
    if (!InRequest) {
        /* nothing deferred outside a request */
    } else if (op == GLCHAN_CB_READ_VIRT) {
        uint8_t *c = pf_find(addr, n);
        if (c) {
            memcpy(p, c, n);
            StPfHits++;
            return 0;
        }
    } else if (op == GLCHAN_CB_WRITE_VIRT && wlog_add(addr, p, n)) {
        return 0;
    }
    uint8_t *b = p;
    while (n) {
        size_t c = MIN(n, (size_t)GLCHAN_DATA_MAX);
        if (op == GLCHAN_CB_WRITE_VIRT || op == GLCHAN_CB_WRITE_PHYS) {
            memcpy((void *)(Chan + GLCHAN_DATA / 4), b, c);
        }
        st(GLCHAN_W_CB_OP, op);
        st(GLCHAN_W_CB_ADDR, addr);
        st(GLCHAN_W_CB_LEN, (uint32_t)c);
        uint32_t seq = ld(GLCHAN_W_CB_SEQ) + 1;
        st(GLCHAN_W_CB_SEQ, seq);
        int64_t tw = Stat ? now_ns() : 0;
        int w = wait_eq(GLCHAN_W_CB_DONE, seq, 10ll * 1000000000);
        if (Stat)
            StCbWaitNs += now_ns() - tw;
        if (w) {
            fprintf(stderr, "glserver: the board did not answer a memory callback (op %d addr %08x)\n", op, addr);
            return -1;
        }
        if (ld(GLCHAN_W_CB_STATUS)) {
            if (CbFailLogs++ < 32) {
                fprintf(stderr, "glserver: the board refused memory op %d at %08x (+%zu)\n", op, addr, c);
            }
            return -1;
        }
        if (op == GLCHAN_CB_READ_VIRT || op == GLCHAN_CB_READ_PHYS) {
            memcpy(b, (const void *)(Chan + GLCHAN_DATA / 4), c);
        }
        StCallbacks++;
        StCbBytes += c;
        addr += c; b += c; n -= c;
    }
    return 0;
}

MemTxResult address_space_read(AddressSpace *as, hwaddr a, MemTxAttrs at, void *p, hwaddr n)
{
    return gs_mem(as->virt ? GLCHAN_CB_READ_VIRT : GLCHAN_CB_READ_PHYS, (uint32_t)a, p, n) ? MEMTX_ERROR : MEMTX_OK;
}

MemTxResult address_space_write(AddressSpace *as, hwaddr a, MemTxAttrs at, const void *p, hwaddr n)
{
    return gs_mem(as->virt ? GLCHAN_CB_WRITE_VIRT : GLCHAN_CB_WRITE_PHYS, (uint32_t)a, (void *)p, n)
           ? MEMTX_ERROR : MEMTX_OK;
}

/* every page "MMIO": syborg_gl.c copies through address_space_read/write (the channel). The page size is
   large so that a big read (a GCE layer) is one callback per MB, not per 4 KB. */
size_t qemu_target_page_size(void) { return 1u << 20; }

int probe_access_full_mmu(CPUARMState *env, uint32_t va, int size, int access, int idx, void **host,
                          CPUTLBEntryFull **full)
{
    static __thread CPUTLBEntryFull f;
    f.phys_addr = va & ~(uint32_t)(qemu_target_page_size() - 1);
    f.attrs = (MemTxAttrs){ .user = 1 };
    *full = &f;
    *host = (void *)&f;                 /* non-NULL; never dereferenced for MMIO pages */
    return TLB_MMIO;
}

/* ---- logging, clocks, timers */
static void vlog(const char *pre, const char *fmt, va_list ap)
{
    if (Quiet) {
        return;
    }
    fprintf(stderr, "glserver: %s", pre);
    vfprintf(stderr, fmt, ap);
    size_t l = strlen(fmt);
    if (!l || fmt[l - 1] != '\n') {
        fputc('\n', stderr);
    }
}
void qemu_log_mask(int mask, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog("", fmt, ap); va_end(ap); }
void warn_report(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog("warning: ", fmt, ap); va_end(ap); }
void info_report(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog("", fmt, ap); va_end(ap); }
void error_report(const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog("error: ", fmt, ap); va_end(ap); }
void error_setg_shim(Error **errp, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vlog("error: ", fmt, ap); va_end(ap); }

int64_t qemu_clock_get_ns(int clock) { return now_ns(); }
int64_t get_clock(void) { return now_ns(); }
int64_t qemu_clock_get_ms(int clock) { return now_ns() / 1000000; }

#define MAX_TIMERS 8
static QEMUTimer *Timers[MAX_TIMERS];
QEMUTimer *timer_new_ms(int clock, void (*cb)(void *), void *opaque)
{
    QEMUTimer *t = g_new0(QEMUTimer, 1);
    t->cb = cb;
    t->opaque = opaque;
    t->deadline_ms = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (!Timers[i]) {
            Timers[i] = t;
            break;
        }
    }
    return t;
}
void timer_mod(QEMUTimer *t, int64_t deadline_ms) { t->deadline_ms = deadline_ms; }
void timer_del(QEMUTimer *t) { t->deadline_ms = -1; }

static void run_timers(void)
{
    int64_t now = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
    for (int i = 0; i < MAX_TIMERS; i++) {
        QEMUTimer *t = Timers[i];
        if (t && t->deadline_ms >= 0 && t->deadline_ms <= now) {
            t->deadline_ms = -1;
            t->cb(t->opaque);
        }
    }
}

static int64_t next_timer_ms(void)
{
    int64_t best = -1;
    for (int i = 0; i < MAX_TIMERS; i++) {
        if (Timers[i] && Timers[i]->deadline_ms >= 0 && (best < 0 || Timers[i]->deadline_ms < best)) {
            best = Timers[i]->deadline_ms;
        }
    }
    return best;
}

/* the composed screen into the channel when it changed (header: syborg_glchan.h, GLCHAN_FRAME) */
static void publish_frame(void)
{
    uint32_t w, h;
    const uint8_t *px;
    if (!syborg_gl_frame(&w, &h, &px)) {
        /* composition stopped (QEMU's syborg-fb shows guest memory again): say so, size 0. Bug 2026-11-12: the
           last frame stayed on the screen, the 808 kept its start-up animation over the language dialog */
        if (ld(GLCHAN_W_FRAME_W)) {
            uint32_t seq = ld(GLCHAN_W_FRAME_SEQ);
            st(GLCHAN_W_FRAME_SEQ, seq | 1);
            st(GLCHAN_W_FRAME_W, 0);
            st(GLCHAN_W_FRAME_H, 0);
            st(GLCHAN_W_FRAME_SEQ, (seq | 1) + 1);
        }
        return;
    }
    size_t bytes = (size_t)w * h * 4;
    uint8_t *dst = (uint8_t *)(uintptr_t)Chan + GLCHAN_FRAME;
    if (bytes && bytes <= GLCHAN_FRAME_MAX &&
        (ld(GLCHAN_W_FRAME_W) != w || ld(GLCHAN_W_FRAME_H) != h || memcmp(dst, px, bytes))) {
        uint32_t seq = ld(GLCHAN_W_FRAME_SEQ);
        st(GLCHAN_W_FRAME_SEQ, seq | 1);            /* odd: being written */
        memcpy(dst, px, bytes);
        st(GLCHAN_W_FRAME_W, w);
        st(GLCHAN_W_FRAME_H, h);
        st(GLCHAN_W_FRAME_SEQ, (seq | 1) + 1);      /* even: whole */
    }
    syborg_gl_frame_done();
}

/* ---- main */
static char *slurp_arg(const char *v) { return v ? g_strdup(v) : NULL; }

int main(int argc, char **argv)
{
    const char *chan = NULL;
    SyborgGlState *s = g_malloc0(sizeof(SyborgGlState));
    syborg_gl_instance_init(OBJECT(s));
    s->thread = false;                  /* GL on the request thread: the board waits for each request */
    s->gpu_mem_mb = 32;
    Stat = getenv("GLSTAT") != NULL;      /* busy and callback-wait time every 2 s on stderr */
    for (int i = 1; i < argc; i++) {
        const char *v = i + 1 < argc ? argv[i + 1] : NULL;
        if (!strcmp(argv[i], "--chan") && v) { chan = v; i++; }
        else if (!strcmp(argv[i], "--identity") && v) { s->identity = slurp_arg(v); i++; }
        else if (!strcmp(argv[i], "--egl-configs") && v) { s->egl_configs = slurp_arg(v); i++; }
        else if (!strcmp(argv[i], "--gpu-memory-mb") && v) { s->gpu_mem_mb = atoi(v); i++; }
        else if (!strcmp(argv[i], "--msaa") && v) { s->msaa = atoi(v); i++; }
        else if (!strcmp(argv[i], "--frame-file") && v) { s->frame_file = slurp_arg(v); i++; }   /* as QEMU's: FSEQ counter */
        else if (!strcmp(argv[i], "--quiet")) { Quiet = 1; }
        else if (!strcmp(argv[i], "--vsync-hz") && v) { VsyncHz = atoi(v); i++; }
        else { fprintf(stderr, "usage: glserver --chan FILE [--identity F] [--egl-configs F] [--gpu-memory-mb N] [--msaa N] [--vsync-hz N]\n"); return 2; }
    }
    if (!chan) {
        fprintf(stderr, "glserver: --chan FILE needed\n");
        return 2;
    }
    int fd = open(chan, O_RDWR | O_CREAT, 0644);
    if (fd < 0 || ftruncate(fd, GLCHAN_BYTES) != 0) {
        perror(chan);
        return 1;
    }
    void *m = mmap(NULL, GLCHAN_BYTES, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
    if (m == MAP_FAILED) {
        perror("mmap");
        return 1;
    }
    Chan = m;
    signal(SIGPIPE, SIG_IGN);
    syborg_gl_realize((DeviceState *)s, NULL);
    st(GLCHAN_W_VERSION, GLCHAN_VERSION);
    st(GLCHAN_W_MAGIC, GLCHAN_MAGIC);
    st(GLCHAN_W_SERVER_UP, 1);
    if (!Quiet) {
        fprintf(stderr, "glserver: syborg-gl device up, channel %s\n", chan);
    }
    uint32_t seen = ld(GLCHAN_W_REQ_SEQ);
    st(GLCHAN_W_RESP_SEQ, seen);
    int64_t last_frame = 0;
    uint32_t last_seq = 0;
    for (unsigned idle = 0;;) {
        /* publish each composed frame as soon as it is complete (its frame-file sequence turns even and new);
           without a frame file, every 16 ms as before. 2026-11-13 (tools/ui_compare.py): the 16 ms poll plus two
           more polling stages showed about half the composed frames in the hosted window */
        int64_t now_ms = qemu_clock_get_ms(QEMU_CLOCK_REALTIME);
        uint32_t fseq = s->frame_seq ? qatomic_read(&s->frame_seq[1]) : 0;
        if (s->frame_seq ? (fseq != last_seq && !(fseq & 1)) : now_ms - last_frame >= 16) {
            last_frame = now_ms;
            last_seq = fseq;
            publish_frame();
        }
        uint32_t seq = ld(GLCHAN_W_REQ_SEQ);
        if (seq == seen) {
            if (ld(GLCHAN_W_MAGIC) != GLCHAN_MAGIC) {
                return 0;               /* the owner cleared the channel: the phone is gone */
            }
            if (next_timer_ms() >= 0) {
                run_timers();
            }
            if (++idle < 4000) {
                __builtin_ia32_pause();
            } else {
                struct timespec d = { 0, idle < 100000 ? 20000 : 1000000 };
                nanosleep(&d, NULL);
            }
            continue;
        }
        idle = 0;
        seen = seq;
        int64_t t_req = Stat ? now_ns() : 0;
        gs_cpu.env.ttbr0 = ld(GLCHAN_W_REQ_PID);
        MemTxAttrs at = { .user = ld(GLCHAN_W_REQ_USER) & 1 };
        uint32_t reg = ld(GLCHAN_W_REQ_REG);
        uint64_t v = 0;
        /* v3: the board's deferred writes, its prefetched memory, an empty write log */
        WlogN = WlogBytes = 0;
        InRequest = 1;
        st(GLCHAN_W_WLOG_N, 0);
        st(GLCHAN_W_WLOG_BYTES, 0);
        PfN = MIN(ld(GLCHAN_W_REQ_NPF), (uint32_t)GLCHAN_PF_ENTRIES);
        for (uint32_t i = 0, npre = MIN(ld(GLCHAN_W_REQ_NPRE), (uint32_t)GLCHAN_PRE_MAX); i < npre; i++) {
            syborg_gl_write(s, ld(GLCHAN_W_PRE + 2 * i), ld(GLCHAN_W_PRE + 2 * i + 1), 4, at);
        }
        if (ld(GLCHAN_W_REQ_OP) == GLCHAN_REQ_READ) {
            syborg_gl_read(s, reg, &v, 4, at);
        } else {
            syborg_gl_write(s, reg, ld(GLCHAN_W_REQ_VALUE), 4, at);
            syborg_gl_read(s, reg, &v, 4, at);      /* v3: the register after the write (KICK: the result) */
            /* a run that failed in a command: say which (the device itself only returns its offset) */
            if ((reg >> 2 == SYGL_REG_KICK || reg >> 2 == SYGL_REG_EXEC) && s->result && s->result != ~0u &&
                FailLogs++ < 64) {
                GlExec *e = &s->exv;
                uint32_t at_w = s->result - 1;
                fprintf(stderr, "glserver: run of %u words failed at word %u:", e->len / 4, at_w);
                for (uint32_t k = at_w; e->cmd && k < at_w + 8 && k < e->len / 4; k++) {
                    fprintf(stderr, " %08x", e->cmd[k]);
                }
                fprintf(stderr, " (owner %u, auth %u/%u)\n", e->owner, e->auth_owner, e->auth_rights);
            }
        }
        StRequests++;
        if (Stat) {
            static int64_t last;
            static uint64_t r0, c0, b0, w0, p0, l0;
            StBusyNs += now_ns() - t_req;
            if (now_ns() - last > 2000000000ll) {
                if (last)
                    fprintf(stderr, "glserver: %lu req/s, busy %lu us/s, of it waiting for callbacks %lu us/s; "
                            "%lu cb/s, %lu prefetch hits/s, %lu logged writes/s\n",
                            (unsigned long)(StRequests - r0) / 2, (unsigned long)(StBusyNs - b0) / 2000,
                            (unsigned long)(StCbWaitNs - w0) / 2000, (unsigned long)(StCallbacks - c0) / 2,
                            (unsigned long)(StPfHits - p0) / 2, (unsigned long)(StWlog - l0) / 2);
                last = now_ns();
                r0 = StRequests; c0 = StCallbacks; b0 = StBusyNs; w0 = StCbWaitNs; p0 = StPfHits; l0 = StWlog;
            }
        }
        InRequest = 0;
        PfN = 0;
        /* an EGL window swap answers at the next vsync, as on the phone (eglSwapBuffers, swap interval 1): the
           requester waits blocked meanwhile (hgl.cpp) and other threads run. 2026-11-13: unpaced, the 808's
           home-screen spinners took the whole CPU and the widgets' loaders never ran */
        if (VsyncHz > 0 && syborg_gl_presents != SeenPresents) {
            SeenPresents = syborg_gl_presents;
            int64_t period = 1000000000ll / VsyncHz, t = now_ns(), next = (t / period + 1) * period;
            while ((t = now_ns()) < next) {
                if (next - t > 200000) {
                    struct timespec d = { 0, next - t - 150000 };
                    nanosleep(&d, NULL);
                }
            }
            StPaced++;
        }
        st(GLCHAN_W_RESP_VALUE, (uint32_t)v);
        st(GLCHAN_W_RESP_SEQ, seq);
    }
}
