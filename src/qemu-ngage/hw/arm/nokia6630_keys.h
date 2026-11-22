/*
 * The Nokia 6630's keypad on the OMAP MPUIO keyboard matrix, as its ekeyp.dll scans it [V Ghidra]: 4 steps
 * drive KBC bit 0..3 low (column words 0x7E 0x7D 0x7B 0x77, table 0x5008147A), 7 row bits (KBR, a pressed key
 * reads 0), scan code = table[step + row * 4] (0x5008145E). An entry is (host key, row, column = step, the
 * scan code it gives); tests/nokia6630_board_test.py checks each against the ROM table.
 * Host keys: digits, * (KP multiply / F7) and # (KP divide / F8), arrows, F1/F2 softkeys, Enter select,
 * F3 call (Yes), F4/Esc end (No), F5 Menu (0xB4, EStdKeyApplication0 [V run: opens the Menu]; F5 = Menu as on the other
 * EKA1 phones), F9 0xAA [I: a side key, nothing visible on the idle screen], Backspace C, F6 edit (pencil) [I].
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA6630_KEYS_H
#define NOKIA6630_KEYS_H

typedef struct {
    int qcode;              /* QKeyCode, Q_KEY_CODE_* */
    unsigned char row, col, scan;
} Rm1Key;

#define RM1_KEYS(Q) { \
    { Q(1), 3, 2, 0x31 }, { Q(2), 2, 1, 0x32 }, { Q(3), 2, 2, 0x33 }, { Q(4), 5, 3, 0x34 }, \
    { Q(5), 3, 0, 0x35 }, { Q(6), 3, 3, 0x36 }, { Q(7), 2, 0, 0x37 }, { Q(8), 2, 3, 0x38 }, \
    { Q(9), 4, 0, 0x39 }, { Q(0), 6, 0, 0x30 }, \
    { Q(KP_MULTIPLY), 4, 1, 0x2a }, { Q(F7), 4, 1, 0x2a }, { Q(KP_DIVIDE), 4, 3, 0x7f }, { Q(F8), 4, 3, 0x7f }, \
    { Q(LEFT), 0, 2, 0x0e }, { Q(RIGHT), 0, 3, 0x0f }, { Q(UP), 1, 2, 0x10 }, { Q(DOWN), 1, 3, 0x11 }, \
    { Q(F1), 0, 0, 0xa4 }, { Q(F2), 0, 1, 0xa5 }, { Q(RET), 1, 0, 0xa7 }, { Q(KP_ENTER), 1, 0, 0xa7 }, \
    { Q(F3), 1, 1, 0xc4 }, { Q(F4), 5, 0, 0xc5 }, { Q(ESC), 5, 0, 0xc5 }, { Q(F9), 5, 1, 0xaa }, \
    { Q(BACKSPACE), 6, 2, 0x01 }, { Q(F6), 6, 1, 0x12 }, { Q(F5), 6, 3, 0xb4 }, \
}

/*
 * The Nokia N70's ekeyp.dll (0x50085970) scans the same matrix with the same column words (0x50086F56) and
 * the same table (0x50086F3A) plus two keys the 6630 does not have: row 4 column 2 = 0xE3 and row 5 column 2
 * = 0xBA [V Ghidra]; which physical keys they are [I] (F10, F11 here).
 */
#define RM84_KEYS(Q) { \
    { Q(1), 3, 2, 0x31 }, { Q(2), 2, 1, 0x32 }, { Q(3), 2, 2, 0x33 }, { Q(4), 5, 3, 0x34 }, \
    { Q(5), 3, 0, 0x35 }, { Q(6), 3, 3, 0x36 }, { Q(7), 2, 0, 0x37 }, { Q(8), 2, 3, 0x38 }, \
    { Q(9), 4, 0, 0x39 }, { Q(0), 6, 0, 0x30 }, \
    { Q(KP_MULTIPLY), 4, 1, 0x2a }, { Q(F7), 4, 1, 0x2a }, { Q(KP_DIVIDE), 4, 3, 0x7f }, { Q(F8), 4, 3, 0x7f }, \
    { Q(LEFT), 0, 2, 0x0e }, { Q(RIGHT), 0, 3, 0x0f }, { Q(UP), 1, 2, 0x10 }, { Q(DOWN), 1, 3, 0x11 }, \
    { Q(F1), 0, 0, 0xa4 }, { Q(F2), 0, 1, 0xa5 }, { Q(RET), 1, 0, 0xa7 }, { Q(KP_ENTER), 1, 0, 0xa7 }, \
    { Q(F3), 1, 1, 0xc4 }, { Q(F4), 5, 0, 0xc5 }, { Q(ESC), 5, 0, 0xc5 }, { Q(F9), 5, 1, 0xaa }, \
    { Q(BACKSPACE), 6, 2, 0x01 }, { Q(F6), 6, 1, 0x12 }, { Q(F5), 6, 3, 0xb4 }, \
    { Q(F10), 4, 2, 0xe3 }, { Q(F11), 5, 2, 0xba }, \
}

#endif
