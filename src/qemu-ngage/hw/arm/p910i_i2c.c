/*
 * Sony Ericsson P910i board (-M p910i): the I2C controller (i2ccontroller.dll), phys 0x58000800,
 * IrqI2C = 17 [V: docs/research/p910i-sound.md s. 2, and the board log: TX 0x134 / reg / val|0x200].
 *   +0x00 TX (16-bit): bit 8 START (with the address byte), bit 9 STOP (with the last byte)
 *   +0x04 STS: bit 0 transfer done (TDI), bit 2 no acknowledge (NAI), write 1 to clear
 *   +0x08 CTL: bits 0/2 interrupt enables; 0x100 soft reset [I]
 *   +0x0C / +0x10 clock dividers / mode [I]: kept
 * Interrupt = (STS & CTL & 5) != 0 [I].
 * H-p910-i2c: every transfer completes at once and is acknowledged; no slave is modelled (the codec
 * at 0x34 [I: write address 0x34] is only written; nothing is read back [V]). Transfers are logged
 * ("p910i-i2c:").
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#include "qemu/osdep.h"
#include "qemu/log.h"
#include "system/memory.h"
#include "system/reset.h"
#include "hw/core/irq.h"
#include "p910i.h"

#define I2C_BASE 0x58000800
#define I2C_IRQ  17

typedef struct {
    MemoryRegion mr;
    qemu_irq irq;
    uint16_t sts, ctl, clk[2];
    uint8_t xfer[16];
    unsigned nx, logged;
} P910I2c;

static void i2c_update(P910I2c *s)
{
    qemu_set_irq(s->irq, (s->sts & s->ctl & 5) != 0);
}

static uint64_t i2c_read(void *opaque, hwaddr a, unsigned size)
{
    P910I2c *s = opaque;

    switch (a) {
    case 0x04: return s->sts;
    case 0x08: return s->ctl;
    case 0x0c: return s->clk[0];
    case 0x10: return s->clk[1];
    }
    return 0;
}

static void i2c_write(void *opaque, hwaddr a, uint64_t v, unsigned size)
{
    P910I2c *s = opaque;

    switch (a) {
    case 0x00:
        if (v & 0x100) {
            s->nx = 0;
        }
        if (s->nx < sizeof(s->xfer)) {
            s->xfer[s->nx++] = v;
        }
        if (v & 0x200) {                    /* STOP: done, acknowledged */
            if (s->logged < 300) {
                char hex[16 * 3 + 1] = "";
                for (unsigned i = 0; i < s->nx; i++) {
                    snprintf(hex + i * 3, 4, " %02x", s->xfer[i]);
                }
                s->logged++;
                qemu_log_mask(LOG_UNIMP, "p910i-i2c:%s pc=%08x\n", hex, p910_pc());
            }
            s->nx = 0;
            s->sts |= 1;
        }
        break;
    case 0x04:
        s->sts &= ~(v & 5);
        break;
    case 0x08:
        if (v & 0x100) {
            s->sts = 0;
            s->nx = 0;
            v &= ~0x100;
        }
        s->ctl = v;
        break;
    case 0x0c:
        s->clk[0] = v;
        break;
    case 0x10:
        s->clk[1] = v;
        break;
    }
    i2c_update(s);
}

static const MemoryRegionOps i2c_ops = {
    .read = i2c_read,
    .write = i2c_write,
    .endianness = DEVICE_LITTLE_ENDIAN,
    .valid = { .min_access_size = 1, .max_access_size = 4 },
};

static void i2c_reset(void *opaque)
{
    P910I2c *s = opaque;

    s->sts = s->ctl = 0;
    s->nx = 0;
    i2c_update(s);
}

void p910_i2c_init(MemoryRegion *sys)
{
    P910I2c *s = g_new0(P910I2c, 1);

    s->irq = p910_irq(I2C_IRQ);
    memory_region_init_io(&s->mr, NULL, &i2c_ops, s, "p910i.i2c", 0x20);
    memory_region_add_subregion_overlap(sys, I2C_BASE, &s->mr, 10);
    qemu_register_reset(i2c_reset, s);
}
