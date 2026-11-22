/*--------------------------------------------------------------.
| q40card.h : Q40/Q60 CF-card & HDD image support for qxltool   |
|                                                                |
| (c) 2026   (extends Jonathan Hudson's qxltool)                 |
| Model: Atari/AHDI partition table in sector 0, up to 4 byte-   |
| swapped QLWA (".WIN") partitions. See q40card_SPEC.md.         |
`--------------------------------------------------------------*/
#ifndef Q40CARD_H
#define Q40CARD_H

#include <stddef.h>
#include <stdint.h>

/* ---- 16-bit byte-swap primitive (Q40 ISA word-window transpose) ---- */
void swab16buf (void *buf, size_t n);   /* n must be even; involution */

/* ---- dispatch: returns 1 if it handled the request, 0 otherwise ---- */
int qxcard_dispatch (int ac, char **av);

#endif
