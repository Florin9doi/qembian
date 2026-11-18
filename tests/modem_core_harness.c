/*
 * Host-side harness for the fake modem core (src/qemu-syborg/hw/arm/syborg_modem_core.c), built as the hosted
 * board builds it (SYBMODEM_HOSTED, src/hosted/include/board/syborg_modem_env.h) with stub host callbacks.
 * Used by tests/modem_unit_test.py (class ModemCoreReplies); no QEMU, no phone.
 *
 *   modem_core_harness CONFIG TABLE WALLSECONDS REQ...
 *
 * CONFIG: the board config string ("sim.present=1;..."), TABLE: a reply table file in the .tab format
 * ("RR QQ PP LEN DATA..", tools/isi.py gen-table) or "-", WALLSECONDS: host UTC for the RTC reply (-1 = no clock).
 * Each REQ is "RES,ID,DATA..." in hex (e.g. "17,0A,03,00"); it is sent with tid 0x42 on ISC channel 7. Every
 * delivered frame is printed as "RES ID TID DATA.." (hex, data = the bytes after the message id).
 *
 * Original work for qembian.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdarg.h>
#include "syborg_modem_core.c"
#undef vsnprintf                        /* the env header maps it to hvsnprintf: here that is the C library's */

int hvsnprintf(char *buf, size_t n, const char *fmt, va_list ap)
{
    return vsnprintf(buf, n, fmt, ap);
}

void *sybmodem_host_alloc_zero(size_t n)
{
    return calloc(1, n);
}

static int64_t wall = -1;

static void h_irq(void *c, int p, int l) {}
static void h_mod(void *c, SybModemTimer *t, int ms) {}
static void h_del(void *c, SybModemTimer *t) {}
static void h_err(void *c, const char *t) { fprintf(stderr, "guest error: %s", t); }
static void h_power(void *c, int w) {}
static bool h_net_present(void *c) { return false; }
static void h_net_send(void *c, const uint8_t *f, size_t n) {}
static void h_net_flush(void *c) {}
static bool h_ring_written(void *c, int s, uint32_t p, uint64_t *t) { return false; }
static uint32_t h_pending(void *c, int s, uint64_t t) { return 0; }
static uint32_t h_ring_size(void *c, int s) { return 0; }
static void h_beep(void *c, uint32_t hz, uint32_t ms, uint32_t g) {}
static int64_t h_wall(void *c) { return wall; }

int main(int argc, char **argv)
{
    static SyborgModemState s;
    static SybModemHost host = { h_irq, h_mod, h_del, h_err, h_power, h_net_present, h_net_send, h_net_flush,
                                 h_ring_written, h_pending, h_ring_size, h_beep, h_wall };
    if (argc < 4) {
        fprintf(stderr, "usage: %s CONFIG TABLE WALLSECONDS REQ...\n", argv[0]);
        return 2;
    }
    wall = strtoll(argv[3], NULL, 10);
    if (wall < 0) {
        host.wall_seconds = NULL;
    }
    sybmodem_init(&s, &host, NULL, argv[1]);
    sybmodem_reset(&s);
    if (strcmp(argv[2], "-")) {
        FILE *f = fopen(argv[2], "r");
        if (!f) {
            perror(argv[2]);
            return 2;
        }
        static char text[1 << 20];
        size_t n = fread(text, 1, sizeof(text) - 1, f);
        fclose(f);
        text[n] = 0;
        sybmodem_load_replies(&s, text);
    }
    const uint32_t ch = 1 << 8 | 7;     /* port 0, handle 1, ISC channel 7 */
    for (int a = 4; a < argc; a++) {
        uint8_t m[256] = { 0 };
        unsigned v[246], n = 0;
        for (char *p = argv[a]; *p && n < 246;) {
            v[n++] = (unsigned)strtoul(p, &p, 16);
            while (*p == ',') {
                p++;
            }
        }
        if (n < 2) {
            return 2;
        }
        m[3] = v[0];
        m[8] = 0x42;
        m[9] = v[1];
        for (unsigned i = 2; i < n; i++) {
            m[10 + i - 2] = v[i];
        }
        uint32_t len = 10 + n - 2;
        m[4] = (len - 6) & 0xff;
        modem_request(&s, ch, m, len);
        ModemPort *p = &s.port[0];
        while (p->count) {
            ModemFrame *f = &p->fifo[p->head];
            p->head = (p->head + 1) % MODEM_FIFO;
            p->count--;
            if (f->kind != RX_DELIVER) {
                continue;
            }
            printf("%02X %02X %02X", f->data[3], f->data[9], f->data[8]);
            for (uint32_t i = 10; i < f->len; i++) {
                printf(" %02X", f->data[i]);
            }
            printf("\n");
        }
    }
    return 0;
}
