/*
 * Nokia 7710 (RM-12): touch screen (ADS7846-class on Microwire CS 0, /PENIRQ on MPUIO 4) and the
 * hardware keys (ekeyp's 5x6 MPUIO matrix) (nokia7710_input.c).
 *
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA7710_INPUT_H
#define NOKIA7710_INPUT_H

struct omap_mpu_state_s;
void rm12_input_init(Object *machine, struct omap_mpu_state_s *mpu);
/* a host key's position on the matrix (row * 8 + col), or -1 */
int rm12_key_pos(unsigned key);

#endif
