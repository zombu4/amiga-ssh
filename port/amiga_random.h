/*
 * amiga_random.h - entropy samples for the AmigaOS port.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#ifndef AMIGA_RANDOM_H
#define AMIGA_RANDOM_H

#include <stddef.h>
#include <exec/types.h>

/* Bytes needed for one full call of amiga_entropy(). */
#define AMIGA_ENTROPY_BYTES 640

/* Fills buf with timing and system samples for the random pool and returns
 * how many bytes it wrote (552 when the timer opens). Its time is spent in 64
 * one-microsecond timer waits: on the reference machine each took 119 to
 * 1724 EClock ticks at 709379 Hz, so the call takes at most about 0.16 s. */
size_t amiga_entropy(unsigned char *buf, size_t size);

/* Low 32 bits of the EClock counter right now, or 0 if timer.device could
 * not be opened. Used to time events such as incoming connections. */
ULONG amiga_eclock_now(void);

#endif /* AMIGA_RANDOM_H */
