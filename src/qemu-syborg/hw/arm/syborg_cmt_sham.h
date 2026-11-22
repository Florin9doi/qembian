/*
 * SHA-1 / MD5 hash accelerator of the N95 modem (RAPIDO), as the modem firmware drives it (n95-cmt probe, map
 * line "sham BASE"; docs/research/n95-cmt-emulation.md section 7). Plain C (no QEMU headers) so that
 * tests/cmt_sham_harness.c can check it against Python's hashlib.
 *
 * Registers (driver FUN_f012eeaa / FUN_f012f06a / FUN_f012f0c4 / FUN_f012f0f8 in MCUSW) [V: driver use]:
 *   +0x00..+0x10  digest A..E: the hash state words (SHA-1: 5, MD5: 4), read and written by the driver to save
 *                 and restore a context
 *   +0x14         byte count: bytes hashed before this block (the driver keeps it; used for the final padding)
 *   +0x18         control: bit 0 output ready (read), bit 1 input ready (read), bit 2 algorithm (1 SHA-1,
 *                 0 MD5), bit 3 first block (start from the algorithm's constants, not A..E), bit 4 close (pad
 *                 and finish the hash), bits 5.. block length in bytes (64 for a full block)
 *   +0x1c..+0x58  data in: 16 words, the block's bytes in memory order (little-endian words)
 *   +0x5c         revision (reads 0x10 [I]); +0x60 mask/sysconfig (kept; bit 1 soft reset); +0x64 sysstatus: bit 0
 *                 reset done (FUN_f012f156 writes +0x60, then waits for it [V])
 * The driver writes control, then the byte count, polls input ready, writes ceil(length / 4) data words from
 * the first one; a full block (64) or the close block is processed once its words are in (a close block of
 * length 0 on the byte-count write). Output ready is set when the block is done.
 * Layout and bits agree with the TI OMAP2 SHA1/MD5 module's public description [I: same IP].
 */
#ifndef SYBORG_CMT_SHAM_H
#define SYBORG_CMT_SHAM_H

#include <stdint.h>
#include <string.h>
#include <stdbool.h>

typedef struct {
    uint32_t digest[5], count, ctrl, din[16], mask;
    unsigned words;             /* data words written since the control write */
    bool pending, ready;
} CmtSham;

#define SHAM_CTRL_OUT_READY 0x01u
#define SHAM_CTRL_IN_READY  0x02u
#define SHAM_CTRL_SHA1      0x04u
#define SHAM_CTRL_FIRST     0x08u
#define SHAM_CTRL_CLOSE     0x10u

static inline uint32_t sham_rol(uint32_t x, unsigned n) { return (x << n) | (x >> (32 - n)); }

static void sham_sha1_block(uint32_t h[5], const uint8_t *p)
{
    uint32_t w[80], a = h[0], b = h[1], c = h[2], d = h[3], e = h[4];
    for (int i = 0; i < 16; i++) {
        w[i] = (uint32_t)p[4 * i] << 24 | (uint32_t)p[4 * i + 1] << 16 | (uint32_t)p[4 * i + 2] << 8 | p[4 * i + 3];
    }
    for (int i = 16; i < 80; i++) {
        w[i] = sham_rol(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);
    }
    for (int i = 0; i < 80; i++) {
        uint32_t f, k;
        if (i < 20) {
            f = (b & c) | (~b & d); k = 0x5a827999;
        } else if (i < 40) {
            f = b ^ c ^ d; k = 0x6ed9eba1;
        } else if (i < 60) {
            f = (b & c) | (b & d) | (c & d); k = 0x8f1bbcdc;
        } else {
            f = b ^ c ^ d; k = 0xca62c1d6;
        }
        uint32_t t = sham_rol(a, 5) + f + e + k + w[i];
        e = d; d = c; c = sham_rol(b, 30); b = a; a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d; h[4] += e;
}

static void sham_md5_block(uint32_t h[4], const uint8_t *p)
{
    static const uint32_t K[64] = {
        0xd76aa478, 0xe8c7b756, 0x242070db, 0xc1bdceee, 0xf57c0faf, 0x4787c62a, 0xa8304613, 0xfd469501,
        0x698098d8, 0x8b44f7af, 0xffff5bb1, 0x895cd7be, 0x6b901122, 0xfd987193, 0xa679438e, 0x49b40821,
        0xf61e2562, 0xc040b340, 0x265e5a51, 0xe9b6c7aa, 0xd62f105d, 0x02441453, 0xd8a1e681, 0xe7d3fbc8,
        0x21e1cde6, 0xc33707d6, 0xf4d50d87, 0x455a14ed, 0xa9e3e905, 0xfcefa3f8, 0x676f02d9, 0x8d2a4c8a,
        0xfffa3942, 0x8771f681, 0x6d9d6122, 0xfde5380c, 0xa4beea44, 0x4bdecfa9, 0xf6bb4b60, 0xbebfbc70,
        0x289b7ec6, 0xeaa127fa, 0xd4ef3085, 0x04881d05, 0xd9d4d039, 0xe6db99e5, 0x1fa27cf8, 0xc4ac5665,
        0xf4292244, 0x432aff97, 0xab9423a7, 0xfc93a039, 0x655b59c3, 0x8f0ccc92, 0xffeff47d, 0x85845dd1,
        0x6fa87e4f, 0xfe2ce6e0, 0xa3014314, 0x4e0811a1, 0xf7537e82, 0xbd3af235, 0x2ad7d2bb, 0xeb86d391 };
    static const uint8_t R[64] = {
        7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 7, 12, 17, 22, 5, 9, 14, 20, 5, 9, 14, 20, 5, 9, 14, 20,
        5, 9, 14, 20, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 4, 11, 16, 23, 6, 10, 15, 21, 6, 10, 15, 21,
        6, 10, 15, 21, 6, 10, 15, 21 };
    uint32_t m[16], a = h[0], b = h[1], c = h[2], d = h[3];
    for (int i = 0; i < 16; i++) {
        m[i] = (uint32_t)p[4 * i] | (uint32_t)p[4 * i + 1] << 8 | (uint32_t)p[4 * i + 2] << 16
               | (uint32_t)p[4 * i + 3] << 24;
    }
    for (int i = 0; i < 64; i++) {
        uint32_t f;
        int g;
        if (i < 16) {
            f = (b & c) | (~b & d); g = i;
        } else if (i < 32) {
            f = (d & b) | (~d & c); g = (5 * i + 1) % 16;
        } else if (i < 48) {
            f = b ^ c ^ d; g = (3 * i + 5) % 16;
        } else {
            f = c ^ (b | ~d); g = (7 * i) % 16;
        }
        uint32_t t = d;
        d = c; c = b;
        b = b + sham_rol(a + f + K[i] + m[g], R[i]);
        a = t;
    }
    h[0] += a; h[1] += b; h[2] += c; h[3] += d;
}

static void sham_process(CmtSham *s)
{
    bool sha1 = s->ctrl & SHAM_CTRL_SHA1;
    unsigned len = (s->ctrl >> 5) & 0x7f;
    uint8_t buf[128];
    if (len > 64) {
        len = 64;
    }
    for (int i = 0; i < 16; i++) {
        buf[4 * i] = s->din[i]; buf[4 * i + 1] = s->din[i] >> 8;
        buf[4 * i + 2] = s->din[i] >> 16; buf[4 * i + 3] = s->din[i] >> 24;
    }
    if (s->ctrl & SHAM_CTRL_FIRST) {
        static const uint32_t iv[5] = { 0x67452301, 0xefcdab89, 0x98badcfe, 0x10325476, 0xc3d2e1f0 };
        memcpy(s->digest, iv, sizeof(iv));
    }
    unsigned nblocks = 1;
    if (s->ctrl & SHAM_CTRL_CLOSE) {    /* pad: 0x80, zeros, the message length in bits (SHA-1 BE, MD5 LE) */
        uint64_t bits = ((uint64_t)s->count + len) * 8;
        memset(buf + len, 0, sizeof(buf) - len);
        buf[len] = 0x80;
        nblocks = len + 9 > 64 ? 2 : 1;
        uint8_t *lp = buf + nblocks * 64 - 8;
        for (int i = 0; i < 8; i++) {
            lp[i] = sha1 ? bits >> (56 - 8 * i) : bits >> (8 * i);
        }
    } else if (len != 64) {             /* only full blocks without close: keep the state */
        nblocks = 0;
    }
    for (unsigned b = 0; b < nblocks; b++) {
        if (sha1) {
            sham_sha1_block(s->digest, buf + 64 * b);
        } else {
            sham_md5_block(s->digest, buf + 64 * b);
        }
    }
    s->pending = false;
    s->ready = true;
}

static inline unsigned sham_words_needed(const CmtSham *s)
{
    unsigned len = (s->ctrl >> 5) & 0x7f;
    return (len > 64 ? 64 : len + 3) / 4;
}

static uint32_t sham_read(CmtSham *s, unsigned off)
{
    if (off < 0x14) {
        return s->digest[off / 4];
    }
    if (off == 0x14) {
        return s->count;
    }
    if (off == 0x18) {
        return (s->ctrl & ~(SHAM_CTRL_OUT_READY | SHAM_CTRL_IN_READY)) | SHAM_CTRL_IN_READY
               | (s->ready ? SHAM_CTRL_OUT_READY : 0);
    }
    if (off >= 0x1c && off < 0x5c) {
        return s->din[(off - 0x1c) / 4];
    }
    if (off == 0x5c) {
        return 0x10;
    }
    if (off == 0x60) {
        return s->mask;
    }
    if (off == 0x64) {
        return 1;               /* reset done */
    }
    return 0;
}

static void sham_write(CmtSham *s, unsigned off, uint32_t v)
{
    if (off < 0x14) {
        s->digest[off / 4] = v;
    } else if (off == 0x14) {
        s->count = v;
        if (s->pending && s->words >= sham_words_needed(s)) {
            sham_process(s);
        }
    } else if (off == 0x18) {
        s->ctrl = v & ~(SHAM_CTRL_OUT_READY | SHAM_CTRL_IN_READY);
        s->words = 0;
        s->pending = true;
        s->ready = false;
        memset(s->din, 0, sizeof(s->din));
    } else if (off >= 0x1c && off < 0x5c) {
        s->din[(off - 0x1c) / 4] = v;
        s->words++;
        if (s->pending && s->words >= sham_words_needed(s) && sham_words_needed(s)) {
            sham_process(s);
        }
    } else if (off == 0x60) {
        s->mask = v & ~2u;      /* the soft reset completes at once */
    }
}

#endif
