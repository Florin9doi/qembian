/*
 * Drives src/qemu-syborg/hw/arm/syborg_cmt_sham.h the way the N95 modem firmware's driver does (FUN_f012ef30 /
 * FUN_f012eeaa / FUN_f012f06a): full 64-byte blocks, then a close block with the remainder (0..64 bytes).
 *   cmt_sham_harness sha1|md5 HEXDATA  ->  prints the digest in hex
 * Used by tests/cmt_probe_test.py (ShamModel), which compares with Python's hashlib.
 */
#include <stdio.h>
#include <stdlib.h>
#include "../src/qemu-syborg/hw/arm/syborg_cmt_sham.h"

static void block(CmtSham *s, const uint8_t *p, unsigned len, uint32_t count, bool first, bool sha1, bool close)
{
    sham_write(s, 0x18, (sha1 ? SHAM_CTRL_SHA1 : 0) | (first ? SHAM_CTRL_FIRST : 0) | (close ? SHAM_CTRL_CLOSE : 0)
               | len << 5);
    sham_write(s, 0x14, count);
    if (!(sham_read(s, 0x18) & SHAM_CTRL_IN_READY)) {
        fprintf(stderr, "input not ready\n");
        exit(1);
    }
    for (unsigned i = 0; i < (len + 3) / 4; i++) {
        uint32_t w = 0;
        for (unsigned k = 0; k < 4 && 4 * i + k < len; k++) {
            w |= (uint32_t)p[4 * i + k] << (8 * k);
        }
        sham_write(s, 0x1c + 4 * i, w);
    }
}

int main(int argc, char **argv)
{
    if (argc != 3) {
        fprintf(stderr, "usage: %s sha1|md5 HEXDATA\n", argv[0]);
        return 2;
    }
    bool sha1 = argv[1][0] == 's';
    size_t n = strlen(argv[2]) / 2;
    uint8_t *m = malloc(n + 1);
    for (size_t i = 0; i < n; i++) {
        sscanf(argv[2] + 2 * i, "%2hhx", &m[i]);
    }
    CmtSham s = { 0 };
    size_t off = 0;
    bool first = true;
    while (n - off > 64) {      /* the driver keeps up to 64 bytes for the close block */
        block(&s, m + off, 64, off, first, sha1, false);
        if (!(sham_read(&s, 0x18) & SHAM_CTRL_OUT_READY)) {
            fprintf(stderr, "block not done\n");
            return 1;
        }
        first = false;
        off += 64;
    }
    block(&s, m + off, n - off, off, first, sha1, true);
    if (!(sham_read(&s, 0x18) & SHAM_CTRL_OUT_READY)) {
        fprintf(stderr, "close block not done\n");
        return 1;
    }
    for (int i = 0; i < (sha1 ? 5 : 4); i++) {
        uint32_t w = sham_read(&s, 4 * i);
        if (sha1) {
            printf("%08x", w);
        } else {
            printf("%02x%02x%02x%02x", w & 0xff, (w >> 8) & 0xff, (w >> 16) & 0xff, w >> 24);
        }
    }
    printf("\n");
    free(m);
    return 0;
}
