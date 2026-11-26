/*
 * OMAP1610 blocks the Nokia 6630 (-M nokia6630) needs on top of QEMU's OMAP310/1510 model.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA6630_SOC_H
#define NOKIA6630_SOC_H

#include "hw/arm/omap.h"

/* The 1610's eight 32-bit GP timers (0xFFFB1400 + 0x800 * i), IRQs as the 1610's interrupt map. */
void rm1_gptimers_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu);

/* display: SoSSI (0xFFFBAC00), the panel (console 0: W x H, 0 = the 6630's 176x208; CTRL = the N90's display
 * controller instead of a DCS panel), the LCD DMA channel (0xFFFEE3C0) */
void rm1_display_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu, unsigned w, unsigned h, bool ctrl);

/* GPIO modules 1-4 (0xFFFBE400, 0xFFFBEC00, 0xFFFBB400, 0xFFFBBC00); module 0..3, line 0..15 */
void rm1_gpio_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu);
void rm1_gpio_set_input(int module, int line, bool level);

/* NAND controller + chip (0xFFFBCC00); PRISTINE = rm1_nand.py image, FLASH = kept copy (or NULL) */
void rm1_nand_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu, const char *pristine,
                   const char *flash);

/* OMAP1710 SPI (0xFFFB0C00, level-2 line 49): transfers complete at once; the panels answer RDDID */
void rm1_spi_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu);

/* 32 kHz watchdog (0xFFFEB000): a register file that never expires */
void rm1_wdt_init(MemoryRegion *sys);

void rm1_gpio_set_output_hook(int module, void (*fn)(void *opaque, uint16_t out), void *opaque);

/* modem link: SSI (0x30000000) + the modem's end of xbus.ldd's protocol; ISI = reply table or NULL */
void rm1_ssi_init(MemoryRegion *sys, struct omap_mpu_state_s *mpu, const char *isi);

/* keypad: host keys to the MPUIO matrix ekeyp.dll scans; N70 = the Nokia N70's key list (two more keys) */
void rm1_keys_init(struct omap_mpu_state_s *mpu, bool n70);

#endif
