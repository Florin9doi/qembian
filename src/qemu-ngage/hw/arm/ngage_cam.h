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
#define CAM_BUF_SIZE      0x97000

/* the sensor's I2C bus: two open-drain lines on the GPIO port 0x0C150200 [V camerapdd 0x503d6b98-0x503d6c04] */
#define CAM_GPIO_SCL      0x0080    /* 0x0C15020C (input) / 0x0C150210 (1 = pull low) */
#define CAM_GPIO_SDA      0x0100
#define CAM_I2C_ADDR      0x3c      /* camerapdd sends 0x78 (write) / 0x79 (read) [V] */
#define CAM_NREGS         0x100     /* 8-bit register numbers: the 7650's camerapdd reads 0..0x7F at power-up
                                       [V 0x503d749c], the 6600's campdd 0..0x9F [V 0x503e0bf8] */

/* CCP receiver, physical 0x0C121000 (registers +0x00..+0x3C) [V camerapdd + its mode table 0x503d8780] */
#define CAM_CCP_BASE      0x0c121000
#define CAM_CCP_SIZE      0x40
#define CAM_CCP_STATUS    0x30      /* R: events, cleared by the read [I: read once per interrupt, never written] */
#define CAM_CCP_MASK      0x34      /* RW: 1 = event masked (0x3FF at set-up, bit 3 cleared to run) [V/I] */
#define CAM_CCP_ENABLE    0x38      /* RW: bit 0 = receiver on (1 at power-up, 0 at power-down) [V] */
#define CAM_CCP_CTRL      0x3c      /* RW: bit 0 pulsed = restart at the next frame; bits 1-2 = 3 for a capture [V] */
#define CAM_ST_FRAME_END  0x0008    /* the ISR's "frame done" bit (0x503d7f7c) [V] */
#define CAM_ST_ERRORS     0x0740    /* treated as a bad frame by the ISR [V] */
#define CAM_CCP_DATA      0x2c      /* R: the receive FIFO, read by the DMA engine only */
#define CAM_CCP_FIFO      0x1012102c    /* the DMA engine's view of +0x2C (alias 0x1012xxxx = 0x0C12xxxx) [V run] */
#define CAM_IRQ           86        /* IrqCCPStatus: interrupt name index 95 - 9 [V ekern 0x5030a9f4] */

typedef struct NgageCam {
    /* I2C: what the master pulls low, what the sensor pulls low */
    bool m_scl_low, m_sda_low, s_sda_low;
    bool scl, sda;                  /* the line levels after the last change */
    int state;                      /* CAM_I2C_* in ngage_cam.c */
    uint8_t shift, nbits, ptr, tx;
    bool rd, first;                 /* read transfer; the next written byte is the register pointer */
    uint8_t regs[CAM_NREGS];
    /* CCP receiver */
    uint32_t ccp[CAM_CCP_SIZE / 4];
    uint16_t status;
    uint32_t frames;                /* frames generated (moves the pattern's marker) */
    void (*log)(void *opaque, const char *msg);
    void *opaque;
} NgageCam;

void ngage_cam_init(NgageCam *c);
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
/* the end of a frame: raise the frame-end event */
void ngage_cam_frame_end(NgageCam *c);
/* the current mode sends 0x0RGB 4444 words (the viewfinder: CCP control bits 1-2 set), else YUV 4:2:2 */
bool ngage_cam_rgb444(const NgageCam *c);
/* a frame of the test pattern as the camera sends it: rgb444 = one little-endian 0x0RGB word per pixel, else
   YUV 4:2:2 (bytes Y1 Cr Y0 Cb per pixel pair); sized from the transfer: 160x120 viewfinder, 176x144 or 640x480
   capture, else 160 wide; frame moves the marker */
void ngage_cam_pattern(uint8_t *dst, uint32_t bytes, uint32_t frame, bool rgb444);
/* the converter camera's Csu.dll applies (0x50a606cc) [V]: YUV pair -> two RGB pixels (for tests) */
void ngage_cam_yuv_to_rgb(const uint8_t q[4], uint8_t rgb0[3], uint8_t rgb1[3]);

#endif
