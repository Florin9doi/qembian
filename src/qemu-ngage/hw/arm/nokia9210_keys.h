/*
 * The Nokia 9210's inner keyboard as EKeyb.dll scans it (docs/research/rae3-keys.md): a 10 x 10 matrix at
 * 0x05013000, +0x20 column drive (bit c low = column c driven), +0x04 rows (active low, 10 bits); source 39
 * ("KeyPress") = any key down. EKeyb's scan code for (column c, row r) is table[c * 10 + r] at 0x508A17F1 [V].
 * An entry: (host QKeyCode, column, row, scan code); tests/nokia9210_board_test.py checks each against the
 * ROM table. Which physical key each code is: mostly [I] (codes per e32keys.h [V]).
 * Command buttons (beside the screen, top to bottom): F1..F4 = Device0..3 (0xA4..0xA7; F1 = the top one, "OK" [V
 * run]); Menu = F9, Help = F10; the application keys App0..7 (0xB4..0xBB: Desk, Telephone, Messaging, ... [I
 * order; F5 = Desk, F6 = Telephone, F7 = Messaging, F8 = Internet [V runs]) = F5..F8, F11, F12, Insert,
 * Delete.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */
#ifndef NOKIA9210_KEYS_H
#define NOKIA9210_KEYS_H

typedef struct {
    int qcode;
    unsigned char col, row, scan;
} Rae3Key;

#define RAE3_KEYS(KEY_) { \
    { KEY_(Q), 8, 1, 'Q' }, { KEY_(W), 8, 2, 'W' }, { KEY_(E), 8, 3, 'E' }, { KEY_(R), 8, 4, 'R' }, { KEY_(T), 8, 5, 'T' }, \
    { KEY_(Y), 8, 6, 'Y' }, { KEY_(U), 8, 7, 'U' }, { KEY_(I), 8, 8, 'I' }, { KEY_(O), 8, 9, 'O' }, { KEY_(P), 7, 9, 'P' }, \
    { KEY_(A), 9, 1, 'A' }, { KEY_(S), 9, 2, 'S' }, { KEY_(D), 9, 3, 'D' }, { KEY_(F), 9, 4, 'F' }, { KEY_(G), 9, 5, 'G' }, \
    { KEY_(H), 9, 6, 'H' }, { KEY_(J), 9, 7, 'J' }, { KEY_(K), 9, 8, 'K' }, { KEY_(L), 9, 9, 'L' }, \
    { KEY_(Z), 1, 6, 'Z' }, { KEY_(X), 6, 3, 'X' }, { KEY_(C), 1, 8, 'C' }, { KEY_(V), 6, 2, 'V' }, { KEY_(B), 1, 9, 'B' }, \
    { KEY_(N), 2, 7, 'N' }, { KEY_(M), 2, 8, 'M' }, \
    { KEY_(1), 7, 0, '1' }, { KEY_(2), 7, 1, '2' }, { KEY_(3), 7, 2, '3' }, { KEY_(4), 7, 3, '4' }, { KEY_(5), 7, 4, '5' }, \
    { KEY_(6), 6, 4, '6' }, { KEY_(7), 7, 5, '7' }, { KEY_(8), 7, 6, '8' }, { KEY_(9), 7, 7, '9' }, { KEY_(0), 7, 8, '0' }, \
    { KEY_(RET), 4, 9, 0x03 }, { KEY_(KP_ENTER), 4, 9, 0x03 }, { KEY_(BACKSPACE), 4, 7, 0x01 }, { KEY_(SPC), 0, 7, 0x05 }, \
    { KEY_(ESC), 5, 0, 0x04 }, { KEY_(TAB), 8, 0, 0x02 }, \
    { KEY_(UP), 3, 6, 0x10 }, { KEY_(RIGHT), 3, 7, 0x0f }, { KEY_(DOWN), 3, 8, 0x11 }, { KEY_(LEFT), 3, 9, 0x0e }, \
    { KEY_(F9), 3, 5, 0x94 }, { KEY_(MENU), 3, 5, 0x94 }, { KEY_(F10), 0, 8, 0x9f }, \
    { KEY_(SHIFT), 1, 7, 0x12 }, { KEY_(CTRL), 6, 1, 0x16 }, { KEY_(ALT), 2, 6, 0x18 }, { KEY_(CAPS_LOCK), 9, 0, 0x1a }, \
    { KEY_(COMMA), 2, 9, 0x79 }, { KEY_(DOT), 4, 8, 0x7a }, { KEY_(SLASH), 0, 9, 0x7b }, \
    { KEY_(F1), 1, 3, 0xa4 }, { KEY_(F2), 0, 3, 0xa5 }, { KEY_(F3), 1, 4, 0xa6 }, { KEY_(F4), 0, 4, 0xa7 }, \
    { KEY_(F5), 5, 1, 0xb4 }, { KEY_(F6), 5, 2, 0xb5 }, { KEY_(F7), 5, 3, 0xb6 }, { KEY_(F8), 5, 4, 0xb7 }, \
    { KEY_(F11), 6, 5, 0xb8 }, { KEY_(F12), 5, 5, 0xb9 }, { KEY_(INSERT), 5, 6, 0xba }, { KEY_(DELETE), 5, 7, 0xbb }, \
}

#endif
