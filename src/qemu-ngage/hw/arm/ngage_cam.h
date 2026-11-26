/*
 * Nokia 7650 (NHL-2NA, EKA1) board: the camera behind camerapdd.pdd, as a test-pattern source.
 * QEMU-free core (the sensor's I2C slave, the UPP's CCP receiver registers, the frame generator); the
 * board glue is in ngage.c (CCP region, frame timer, interrupt 86), ngage_keys.c (the I2C pins on the
 * GPIO port 0x0C150200) and ngage_mmc.c (the DMA engine moving frames from the CCP FIFO).
 * Host unit test: tests/ngage_cam_test.c. Protocol: docs/devices/nokia-7650-nhl2/camera.md.
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NGAGE_CAM_H
#define NGAGE_CAM_H

#include <stdint.h>
#include <stdbool.h>

/* the camera buffer: the variant's hardware chunk at SDRAM physical 0x0A000000, 0x96000 bytes on the 7650 (ecust
   0x5032fcd0), 0x97000 on the 6600 (0x5033a0d8) and the 3650/3660 [V] */
#define CAM_BUF_PHYS      0x0a000000
#define CAM_BUF_SIZE      0xed82c   /* the largest transfer: the 7610's JPEG still (CAM_CAMX_STILL); the 7650 0x96000,
                                       the 6600 and 3650/3660 0x97000 */
#define CAM_CAMX_STILL    0xed82c   /* 7610/6670: mode 5 of camxpdd's tables (0x503e7c64 / 0x503e7ee4, 40-byte
                                       entries: +0x20 expected size, +0x24 DMA size): expected 0 = any length, DMA
                                       0xED82C = the header and up to 0xED800 bytes of JPEG. The other modes are raw
                                       with exact sizes: 160x120, 320x240, 640x480, 128x96, 176x144, 352x288 [V] */
#define CAM_CAMX_W        1152      /* the 7610's still size (1 megapixel) [I] */
#define CAM_CAMX_H        864
#define CAM_HDR_CAMX      44        /* 7610/6670 camxpdd: each frame starts with 44 bytes the driver skips (ISR
                                       0x503e5600: count > 44, picture size = count - 44); content not read [I] */

/* the sensor's I2C bus: two open-drain lines on the GPIO port 0x0C150200 [V camerapdd 0x503d6b98-0x503d6c04] */
#define CAM_GPIO_SCL      0x0080    /* 0x0C15020C (input) / 0x0C150210 (1 = pull low) */
#define CAM_GPIO_SDA      0x0100
#define CAM_I2C_ADDR      0x3c      /* 7650/6600: camerapdd sends 0x78 (write) / 0x79 (read) [V] */
#define CAM_I2C_ADDR_RM51 0x30      /* 3230: 0x60 / 0x61 (campdd 0x5040880c / 0x50408914) [V] */
#define CAM_CMD_PORT      0xfe      /* 3230: the module's command / answer register [V] */
#define CAM_I2C_ADDR_NHL8 0x04      /* 3650/3660: 0x08 / 0x09, 16-bit register numbers (high byte first) [V
                                       camerapdd 0x5040c780 / 0x503e8908] */
#define CAM_NREGS         0x10000   /* 8-bit register numbers on the 7650 (reads 0..0x7F at power-up, 0x503d749c)
                                       and the 6600 (0..0x9F, 0x503e0bf8); 16-bit on the 3650/3660 [V] */
#define CAM_REG_MODE      0xc005    /* 3650/3660: 2 viewfinder (12-bit screen: RGB 4444, CCP control 6), 6 still,
                                       7 viewfinder for a 16-bit screen (3660): RGB565, copied into the bitmap as it
                                       is (3660 Csu.dll 0x50aa0c88, display mode 7) [V; little-endian: V run] */
#define CAM_REG_JPEG      0xc102    /* 3650/3660: 6 = the sensor compresses stills to JPEG, 4 = raw (mode tables
                                       0x5040ee24 / 0x503ea350, states 7-9 vs 4) [V] */

/* CCP receiver, physical 0x0C121000 (registers +0x00..+0x3C) [V camerapdd + its mode table 0x503d8780] */
#define CAM_CCP_BASE      0x0c121000
#define CAM_CCP_SIZE      0x40
#define CAM_CCP_STATUS    0x30      /* R: events, cleared by the read [I: read once per interrupt, never written] */
#define CAM_CCP_MASK      0x34      /* RW: 1 = event masked (0x3FF at set-up, bit 3 cleared to run) [V/I] */
#define CAM_CCP_ENABLE    0x38      /* RW: bit 0 = receiver on (1 at power-up, 0 at power-down) [V] */
#define CAM_CCP_CTRL      0x3c      /* RW: bit 0 pulsed = restart at the next frame; bits 1-2 = 3 for a capture [V] */
#define CAM_ST_FRAME_END  0x0008    /* the ISR's "frame done" bit (0x503d7f7c) [V] */
#define CAM_ST_FRAME_START 0x0010   /* 7610/6670 camxpdd: a frame begins; its ISR (0x503e5600) queues the DFC that arms
                                       the next buffer on it, and takes bits 3 and 4 together as an error [V code] */
#define CAM_ST_ERRORS     0x0740    /* treated as a bad frame by the ISR [V] */
#define CAM_CCP_DATA      0x2c      /* R: the receive FIFO, read by the DMA engine only */
#define CAM_CCP_FIFO      0x1012102c    /* the DMA engine's view of +0x2C (alias 0x1012xxxx = 0x0C12xxxx) [V run] */
#define CAM_IRQ           86        /* IrqCCPStatus: interrupt name index 95 - 9 [V ekern 0x5030a9f4] */

typedef struct NgageCam {
    /* I2C: what the master pulls low, what the sensor pulls low */
    bool m_scl_low, m_sda_low, s_sda_low;
    bool scl, sda;                  /* the line levels after the last change */
    int state;                      /* CAM_I2C_* in ngage_cam.c */
    uint8_t shift, nbits, tx;
    uint16_t ptr;
    bool rd;                        /* a read transfer */
    uint8_t naddr;                  /* register-pointer bytes still to come in this write */
    uint8_t addr;                   /* the sensor's 7-bit I2C address (CAM_I2C_ADDR*) */
    bool reg16;                     /* 16-bit register numbers (3650/3660) */
    bool cmdport;                   /* 3230: a camera module behind a command port (register 0xFE): 8-byte commands
                                       in, 8-byte answers out (ngage_cam_cmd) */
    uint8_t cmd[8], ncmd, resp[8], nresp;
    bool camx;                      /* 7610/6670 camxpdd: frames carry a 44-byte header; stills are a JPEG in the
                                       sensor's transport (ngage_cam_camx_jpeg), its length in registers 0xF6-0xF8 */
    bool rot90;                     /* the sensor lies on its side (6260): frames carry the picture turned 90 degrees
                                       clockwise (set by the board after init; ngage_cam_fill) */
    uint8_t regs[CAM_NREGS];
    /* CCP receiver */
    uint32_t ccp[CAM_CCP_SIZE / 4];
    uint16_t status;
    uint32_t frames;                /* frames generated (moves the pattern's marker) */
    void (*log)(void *opaque, const char *msg);
    void *opaque;
} NgageCam;

/* addr / reg16: the sensor's I2C address and register width (set again after init by the board) */
void ngage_cam_init(NgageCam *c);
/* 3230: the answer to a module command */
void ngage_cam_cmd(NgageCam *c, const uint8_t *cmd, unsigned n, uint8_t resp[8]);
void ngage_cam_set_sensor(NgageCam *c, uint8_t addr, bool reg16);
/* GPIO port: the value written to 0x0C150210 (bits 7/8 = pull SCL/SDA low) */
void ngage_cam_gpio_drive(NgageCam *c, uint16_t drive);
/* GPIO port: the bus lines as 0x0C15020C shows them (bits 7/8; 1 = high) */
uint16_t ngage_cam_gpio_lines(const NgageCam *c);
uint32_t ngage_cam_ccp_read(NgageCam *c, unsigned off);
void ngage_cam_ccp_write(NgageCam *c, unsigned off, uint32_t val);
/* the receiver is on: frames arrive */
bool ngage_cam_streaming(const NgageCam *c);
/* the CCP interrupt line (an unmasked event waits) */
bool ngage_cam_irq(const NgageCam *c);
/* the start of a frame: raise the frame-start event, only when unmasked (the 7650's family masks it and its
   ISR would see it at the next frame end) */
void ngage_cam_frame_start(NgageCam *c);
/* the end of a frame: raise the frame-end event */
void ngage_cam_frame_end(NgageCam *c);
/* the current mode sends 0x0RGB 4444 words (the viewfinder: CCP control bits 1-2 set), else YUV 4:2:2 */
bool ngage_cam_rgb444(const NgageCam *c);
/* a frame of the test pattern as the camera sends it: rgb444 = one little-endian 0x0RGB word per pixel, else
   YUV 4:2:2 (bytes Y1 Cr Y0 Cb per pixel pair); sized from the transfer: 160x120 viewfinder, 176x144 or 640x480
   capture, else 160 wide; frame moves the marker */
void ngage_cam_pattern(uint8_t *dst, uint32_t bytes, uint32_t frame, bool rgb444);
/*
 * A frame as the current mode sends it, into a DMA transfer of `bytes`:
 *   - RGB444 (CCP control bits 1-2 set), else RGB565 (16-bit sensor with CAM_REG_MODE = 7), else JPEG (16-bit sensor
 *     with CAM_REG_JPEG = 6), else YUV 4:2:2 (the
 *     7650/6600's Y1 Cr Y0 Cb, the 3650/3660's Cr Y1 Cb Y0: 16-bit sensor);
 *   - layout from the size: w x h x 2 (7650, 6600), or w x (h + 2) x 2 = a header line, the picture and a spare line
 *     (3650/3660: 160x122, 640x482; the driver hands out the buffer + one line [V 0x5040db2c +0x140]), or a 44-byte
 *     header then w x h x 2 (7610/6670: CAM_HDR_CAMX);
 *   - JPEG: two bytes of the file per 32-bit word, at byte offsets 3 and 2 (the driver compacts them, 0x5040e87c) [V].
 */
uint32_t ngage_cam_fill(NgageCam *c, uint8_t *dst, uint32_t bytes);     /* returns the bytes sent (<= bytes) */
/*
 * 7610/6670 still (camxpdd, the CAM_CAMX_STILL transfer): a CAM_CAMX_W x CAM_CAMX_H JPEG in the sensor's transport, as camxpdd 0x503e6258 /
 * 0x503e6290 undo it [V]: an 0xA5 after every 0x00 that follows two bytes (a, b) with b = 2^n - 1 and a | b = 0xFF
 * (the driver drops it; unescaped, 00 there ends the picture), padded with 0xA5 to whole words (at most 3), each
 * 32-bit word byte-reversed. Writes the stream at dst (cap bytes), returns its length (0 = no room) and the JPEG's
 * length in *jpeg_len.
 */
uint32_t ngage_cam_camx_jpeg(uint8_t *dst, uint32_t cap, uint32_t frame, uint32_t *jpeg_len);
#define CAM_REG_CAMX_LEN  0xf6      /* 7610/6670: 0xF6-0xF8 the still's JPEG length (high byte first), 0xFA status:
                                       bits 7 and 6 set = good (camxpdd 0x503e5e6c) [V] */
/* a baseline JPEG (YCbCr 4:2:2, quality 75, the standard Huffman tables) of the test pattern; its length, 0 = no room */
uint32_t ngage_cam_jpeg(uint8_t *out, uint32_t cap, unsigned w, unsigned h, uint32_t frame);
/* the converter camera's Csu.dll applies (0x50a606cc) [V]: YUV pair -> two RGB pixels (for tests) */
void ngage_cam_yuv_to_rgb(const uint8_t q[4], uint8_t rgb0[3], uint8_t rgb1[3]);
/* the 3650/3660 Csu.dll converter (0x50abfee0) [V]: bytes Cr Y1 Cb Y0 -> two RGB pixels (for tests) */
void ngage_cam_yuv8_to_rgb(const uint8_t q[4], uint8_t rgb0[3], uint8_t rgb1[3]);

#endif
