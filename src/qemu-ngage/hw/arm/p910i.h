/*
 * Sony Ericsson P910i board (-M p910i): shared declarations of p910i*.c.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_P910I_H
#define HW_ARM_P910I_H

#include "hw/core/boards.h"
#include "target/arm/cpu.h"
#include "hw/core/irq.h"

#define P910_RAM_BASE  0xD0000000
#define P910_RAM_SIZE  (32 * MiB)
#define P910_SRAM_BASE 0x68000000
#define P910_SRAM_SIZE (64 * KiB)

uint32_t p910_pc(void);
const char *p910_images(void);
const char *p910_isi_file(void);
const char *p910_flash_file(void);
bool p910_patch(uint8_t *nor, uint32_t va, const void *old, size_t oldlen, const void *new, size_t len);
/* the SoC's devices (p910i_soc.c) */
/* interrupt line 0..63 of the two interrupt controllers */
qemu_irq p910_irq(int line);
/* the GSM link on UART unit 2 (p910i_gsm.c) */
void p910_gsm_init(MemoryRegion *sys);
/* display path, DMA (p910i_lcd.c) */
void p910_lcd_init(MemoryRegion *sys);
/* I2C controller (p910i_i2c.c) */
void p910_i2c_init(MemoryRegion *sys);
/* input: touch, keypad panel, jog, side key, flip (p910i_input.c) */
void p910_input_init(MachineState *machine, MemoryRegion *sys);
uint32_t p910_touch_raw(int px, int py);
/* GPIO input pins of the sys page (p910i_soc.c) */
void p910_gpio_set(uint32_t mask, uint32_t value);
void p910_board_init(MachineState *machine, MemoryRegion *sys, ARMCPU *cpu);

#endif
