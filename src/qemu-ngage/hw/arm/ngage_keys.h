/* N-Gage (EKA1) board: the keypad (ngage_keys.c). */
#ifndef NGAGE_KEYS_H
#define NGAGE_KEYS_H

typedef struct {
    unsigned host;              /* Linux key number (QEMU input layer) */
    const char *name;
    int row, col;               /* position in ekeyb's matrix; -1 = not in it; -2 = 6600 joystick bit */
} NgageKey;

/* nhl10: the Nokia 6600 key table, GPIO sense line and joystick port */
void ngage_keys_init(MemoryRegion *sys, qemu_irq irq, bool nhl10);
/* CTSI status 2 (0x0C150102): raise bits (bit 4 = Bluetooth module), same interrupt source */
void ngage_ctsi2_raise(uint16_t bits);

#endif
