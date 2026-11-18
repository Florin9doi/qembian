/*
 * Nokia 9500 (RA-2): the keyboard controller on I2C 0x56 (keys, lid) (nokia9500_kbd.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA9500_KBD_H
#define NOKIA9500_KBD_H

struct omap_mpu_state_s;
void ra2_kbd_init(Object *machine, struct omap_mpu_state_s *mpu);
void ra2_kbd_push(uint8_t b);
/* the controller byte for a host key (Linux input code) going down or up, or -1 (not a 9500 key) */
int ra2_kbd_byte(unsigned key, bool down);
/* a host key's position on the MPUIO keypad matrix (row * 8 + col), or -1; cover keys only with the lid closed */
int ra2_keypad_pos(unsigned key, bool lid_open);

/* the TLV320AIC23 audio codec on I2C 0x1A (registers kept, no audio) */
void ra2_codec_init(struct omap_mpu_state_s *mpu);

#endif
