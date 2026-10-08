/*
 * amiga_random.c - entropy samples for the AmigaOS port.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * AmigaOS has no /dev/urandom. Dropbear reads its random device once at
 * start and writes fresh output back to it when seeding is done
 * (dbrandom.c:223-310), so the port points that path at a seed file
 * (AMIGA_SSHD_SEED) and adds the samples gathered here to the same SHA-256
 * pool. Nothing here is treated as random by itself: the samples are hashed
 * together with the seed file, and the pool carries over from one start to
 * the next.
 *
 * Samples:
 *  - how long each of a series of 1-microsecond timer.device waits really
 *    takes, read with ReadEClock(); the wake-up time depends on interrupts,
 *    DMA and task switching;
 *  - how long a short fixed computation takes, read the same way;
 *  - the date stamp, the free memory and this task's address.
 */
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <proto/timer.h>

#include "amiga_random.h"

struct Device *TimerBase;

#define WAIT_SAMPLES 64
#define WORK_SAMPLES 64

/* Appends len bytes to buf if they fit; returns the new fill level. */
static size_t put(unsigned char *buf, size_t size, size_t used,
		const void *data, size_t len)
{
	if (used + len > size)
		return used;
	memcpy(buf + used, data, len);
	return used + len;
}

/* Low 32 bits of the EClock difference b - a. */
static ULONG eclock_delta(const struct EClockVal *a, const struct EClockVal *b)
{
	return b->ev_lo - a->ev_lo;
}

size_t amiga_entropy(unsigned char *buf, size_t size)
{
	struct MsgPort *port;
	struct timerequest *tr;
	struct EClockVal before, after;
	struct DateStamp ds;
	ULONG value, freq;
	APTR task;
	size_t used = 0;
	int i;
	volatile ULONG work = 0;

	DateStamp(&ds);
	used = put(buf, size, used, &ds, sizeof(ds));
	value = AvailMem(MEMF_ANY);
	used = put(buf, size, used, &value, sizeof(value));
	value = AvailMem(MEMF_CHIP);
	used = put(buf, size, used, &value, sizeof(value));
	task = FindTask(NULL);
	used = put(buf, size, used, &task, sizeof(task));

	port = CreateMsgPort();
	if (port == NULL)
		return used;
	tr = (struct timerequest *)CreateIORequest(port, sizeof(*tr));
	if (tr == NULL) {
		DeleteMsgPort(port);
		return used;
	}
	if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
			(struct IORequest *)tr, 0) != 0) {
		DeleteIORequest((struct IORequest *)tr);
		DeleteMsgPort(port);
		return used;
	}
	TimerBase = tr->tr_node.io_Device;

	freq = ReadEClock(&before);
	used = put(buf, size, used, &freq, sizeof(freq));
	used = put(buf, size, used, &before, sizeof(before));

	for (i = 0; i < WAIT_SAMPLES; i++) {
		ReadEClock(&before);
		tr->tr_node.io_Command = TR_ADDREQUEST;
		tr->tr_time.tv_secs = 0;
		tr->tr_time.tv_micro = 1;
		DoIO((struct IORequest *)tr);
		ReadEClock(&after);
		value = eclock_delta(&before, &after);
		used = put(buf, size, used, &value, sizeof(value));
	}

	for (i = 0; i < WORK_SAMPLES; i++) {
		ULONG n;

		ReadEClock(&before);
		for (n = 0; n < 200; n++)
			work = work * 1664525UL + 1013904223UL;
		ReadEClock(&after);
		value = eclock_delta(&before, &after);
		used = put(buf, size, used, &value, sizeof(value));
	}
	value = work;
	used = put(buf, size, used, &value, sizeof(value));

	CloseDevice((struct IORequest *)tr);
	TimerBase = NULL;
	DeleteIORequest((struct IORequest *)tr);
	DeleteMsgPort(port);
	return used;
}

ULONG amiga_eclock_now(void)
{
	struct MsgPort *port;
	struct timerequest *tr;
	struct EClockVal now;

	now.ev_hi = 0;
	now.ev_lo = 0;
	port = CreateMsgPort();
	if (port == NULL)
		return 0;
	tr = (struct timerequest *)CreateIORequest(port, sizeof(*tr));
	if (tr != NULL) {
		if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_MICROHZ,
				(struct IORequest *)tr, 0) == 0) {
			TimerBase = tr->tr_node.io_Device;
			ReadEClock(&now);
			CloseDevice((struct IORequest *)tr);
			TimerBase = NULL;
		}
		DeleteIORequest((struct IORequest *)tr);
	}
	DeleteMsgPort(port);
	return now.ev_lo;
}
