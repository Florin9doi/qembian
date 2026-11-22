/*
 * Sony Ericsson P800: the LFFS NOR at phys 0xB0000000 (p800_nor.c).
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef HW_ARM_P800_NOR_H
#define HW_ARM_P800_NOR_H

/* dir: the images (lng.bin, rolf.bin); flash: the phone's kept NOR image or NULL */
void p800_nor_init(MemoryRegion *sys, const char *dir, const char *flash);

#endif
