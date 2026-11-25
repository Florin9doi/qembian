/* N-Gage (EKA1) board: the keypad (ngage_keys.c). */
#ifndef NGAGE_KEYS_H
#define NGAGE_KEYS_H

typedef struct {
    unsigned host;              /* Linux key number (QEMU input layer) */
    const char *name;
    int row, col;               /* position in ekeyb's matrix; -1 = not in it; -2 = 6600 joystick bit */
} NgageKey;

/* nhl10: the Nokia 6600 key table, GPIO sense line and joystick port */
/* key layouts: the N-Gage's matrix, the 6600's (two sub-matrices + joystick port; also the 7650), the 3230's */
typedef enum { NGAGE_KPD_NGAGE, NGAGE_KPD_NHL8X, NGAGE_KPD_NHL10, NGAGE_KPD_RM51 } NgageKpdLayout;
void ngage_keys_init(MemoryRegion *sys, qemu_irq irq, NgageKpdLayout layout);
/* CTSI status 2 (0x0C150102): raise bits (bit 4 = Bluetooth module), same interrupt source */
void ngage_ctsi2_raise(uint16_t bits);
/* 7650: a device on the joystick port's free lines (the camera's I2C pins: 0x0C15020C bits 7/8 in,
   0x0C150210 written = which lines it pulls low) */
void ngage_keys_gpio_peer(uint16_t (*lines)(void *opaque), void (*drive)(void *opaque, uint16_t drive),
                          void *opaque);
/* 7650: the keypad slide on the switch line (0x0C15020C bit 9); present, and its position */
void ngage_keys_slide(bool present, bool closed);
bool ngage_keys_slide_closed(void);

#endif
