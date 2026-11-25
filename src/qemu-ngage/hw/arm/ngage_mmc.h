/* N-Gage QD board: MMC host controller + DMA engine (ngage_mmc.c). SPDX-License-Identifier: GPL-2.0-or-later */
#ifndef NGAGE_MMC_H
#define NGAGE_MMC_H
#include "system/memory.h"
void ngage_mmc_init(MemoryRegion *sys, qemu_irq *intc, const char *card);

/*
 * Another device on the DMA engine (the 7650's camera: its CCP FIFO). start() is asked about every device-paced
 * transfer that does not touch the MMC FIFO; true = the peer owns it: the channel stays busy (control reads back)
 * until the peer calls ngage_dma_finish(), which leaves the remaining count at -1 (all moved) and the channel idle.
 * The channel's own interrupt is not raised for a peer's transfer [I: camerapdd's DMA callback does nothing and its
 * CCP interrupt stops the channel]. stop() = the driver wrote control 0.
 */
typedef struct {
    bool (*start)(void *opaque, int ch, uint32_t src, uint32_t dst, uint32_t bytes);
    void (*stop)(void *opaque, int ch);
    void *opaque;
} NgageDmaPeer;
void ngage_dma_set_peer(const NgageDmaPeer *peer);
void ngage_dma_finish(int ch);
#endif
