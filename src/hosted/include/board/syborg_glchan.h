/*
 * The syborg-gl channel between the hosted EKA2 board (src/hosted/variant/hgl.cpp, a 32-bit process in the
 * test VM or on the phone) and the host GL server (src/hosted/glserver, a 64-bit process with the GPU). One
 * shared memory area (an ivshmem BAR in the VM; a shared file / memfd on the phone), little-endian u32 words.
 * Both sides poll; nothing else is shared. Original work for qembian.
 * SPDX-License-Identifier: GPL-2.0-or-later OR EPL-1.0
 *
 *  Register requests (the hosted kernel's ReadReg/WriteReg of syborg-gl, one at a time):
 *    the board fills REQ_* and bumps REQ_SEQ; the server runs syborg_gl.c's MMIO op and writes RESP_VALUE,
 *    then sets RESP_SEQ = REQ_SEQ.
 *  Memory callbacks (the server needs guest memory while it runs a request, or later for a frame):
 *    the server fills CB_* (+ data for writes at GLCHAN_DATA) and bumps CB_SEQ; the board copies and sets
 *    CB_STATUS and CB_DONE = CB_SEQ. Virtual addresses are the hosted process's (one address space);
 *    physical ones are the board's physical windows (driver RAM: frame buffers, command buffers).
 *  v3, fewer round trips (2026-11-13: ~180 requests and ~300 callbacks per 808 frame, 100-150 us each):
 *    - deferred writes: REQ_NPRE (reg, value) pairs at W_PRE, written before the request's own op (the board holds
 *      writes to plain registers: BUF LEN OUT OUTLEN PID FLAGS); after a write the server reads the same register
 *      into RESP_VALUE (KICK/EXEC/BIND: the result, which the board keeps for the read that follows);
 *    - prefetch: REQ_NPF entries {va, len, offset} at W_PF: the board copied that virtual memory to GLCHAN_PF +
 *      offset; the server's reads inside an entry are served from there, not by a callback;
 *    - write log: the server's virtual-memory writes go to GLCHAN_WLOG as {va, len, data (padded to 4)} and
 *      W_WLOG_N counts them; the board applies them after the response and before serving any callback (order
 *      kept); a write that does not fit is a callback.
 *  The composed screen (syborg_gl_frame: what QEMU's syborg-fb shows instead of guest memory when the guest
 *  composes on the host): the server copies it to GLCHAN_FRAME when it changed (at most every 16 ms) with
 *  FRAME_W/H (0 x 0: composition stopped, the board shows video RAM again); FRAME_SEQ is odd while it writes
 *  and even when the frame is whole (0: none yet). The board
 *  reads it as the panel's picture (hboard.cpp HostedFbScreen), re-reading when FRAME_SEQ moved meanwhile.
 */
#ifndef SYBORG_GLCHAN_H
#define SYBORG_GLCHAN_H

#define GLCHAN_MAGIC        0x434c4753u     /* "SGLC" */
#define GLCHAN_VERSION      3
#define GLCHAN_BYTES        (64u << 20)     /* the whole area */
#define GLCHAN_DATA         0x10000u        /* callback data */
#define GLCHAN_WLOG         (24u << 20)     /* the server's virtual writes of the request (v3) */
#define GLCHAN_WLOG_MAX     (8u << 20)
#define GLCHAN_PF           (32u << 20)     /* the board's prefetched virtual memory (v3) */
#define GLCHAN_PF_MAX       (24u << 20)
#define GLCHAN_PF_ENTRIES   16
#define GLCHAN_PRE_MAX      8
#define GLCHAN_FRAME        (56u << 20)     /* the composed screen, XRGB words, FRAME_W x FRAME_H */
#define GLCHAN_FRAME_MAX    (GLCHAN_BYTES - GLCHAN_FRAME)
#define GLCHAN_DATA_MAX     (GLCHAN_WLOG - GLCHAN_DATA)

enum {
    /* words (index = byte offset / 4) */
    GLCHAN_W_MAGIC = 0, GLCHAN_W_VERSION, GLCHAN_W_SERVER_UP,    /* server: 1 when its device is ready */
    GLCHAN_W_REQ_SEQ = 8, GLCHAN_W_REQ_OP, GLCHAN_W_REQ_REG, GLCHAN_W_REQ_VALUE, GLCHAN_W_REQ_USER,
    GLCHAN_W_REQ_PID,                       /* the hosted process (the identity a kick/BIND binds) */
    GLCHAN_W_RESP_SEQ = 16, GLCHAN_W_RESP_VALUE,
    GLCHAN_W_CB_SEQ = 24, GLCHAN_W_CB_OP, GLCHAN_W_CB_ADDR, GLCHAN_W_CB_LEN, GLCHAN_W_CB_DONE, GLCHAN_W_CB_STATUS,
    GLCHAN_W_CB_CLAIM,                      /* board: who serves this callback (requester or helper thread) */
    GLCHAN_W_FRAME_SEQ = 32, GLCHAN_W_FRAME_W, GLCHAN_W_FRAME_H,
    GLCHAN_W_REQ_NPRE = 36, GLCHAN_W_REQ_NPF, GLCHAN_W_WLOG_N, GLCHAN_W_WLOG_BYTES,
    GLCHAN_W_PRE = 40,                      /* GLCHAN_PRE_MAX (reg, value) pairs */
    GLCHAN_W_PF = 96,                       /* GLCHAN_PF_ENTRIES {va, len, offset} */
    GLCHAN_W_STATS = 64                     /* board: requests, callbacks, bytes (for tests and tuning) */
};
enum { GLCHAN_REQ_READ = 1, GLCHAN_REQ_WRITE = 2 };
enum { GLCHAN_CB_READ_VIRT = 1, GLCHAN_CB_WRITE_VIRT, GLCHAN_CB_READ_PHYS, GLCHAN_CB_WRITE_PHYS };

#endif
