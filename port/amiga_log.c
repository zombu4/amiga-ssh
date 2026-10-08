/*
 * amiga_log.c - the server's log file on AmigaOS.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Dropbear prints every log line to standard error (svr-session.c,
 * svr_dropbear_log; syslog is compiled out). The listener and the sessions
 * run without a console (their output is NIL:), so patch 0010 also hands
 * each line to amiga_log_line(), which appends it to AMIGA_SSHD_LOG.
 *
 * Size limit: before a line is added, a log that has reached
 * AMIGA_SSHD_LOG_MAX bytes is renamed to AMIGA_SSHD_LOG_OLD (the previous
 * one is deleted first, as Rename() refuses to overwrite), so the two files
 * together stay near twice that size.
 *
 * The listener and several sessions write to the same file. Each line is
 * written with its own Open(MODE_READWRITE) - a shared lock, so the
 * processes do not shut each other out - a Seek() to the end, one Write()
 * and Close(). Only dos.library is used: libnix's file I/O calls
 * __chkabort(), which can end the program with exit(20) on Ctrl-C
 * (sources/nix/stdio/write.c, nix/signal/raise.c). A line that cannot be
 * written is dropped; logging never stops the server.
 */
#include <string.h>

#include <exec/types.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "amiga_paths.h"
#include "amiga_log.h"

/* Renames a full log to the .old name. */
static void rotate_if_full(void)
{
	struct FileInfoBlock *fib;
	BPTR lock;
	LONG size = 0;

	lock = Lock((CONST_STRPTR)AMIGA_SSHD_LOG, SHARED_LOCK);
	if (lock == 0)
		return;
	fib = AllocDosObject(DOS_FIB, NULL);
	if (fib != NULL) {
		if (Examine(lock, fib))
			size = fib->fib_Size;
		FreeDosObject(DOS_FIB, fib);
	}
	UnLock(lock);
	if (size < AMIGA_SSHD_LOG_MAX)
		return;
	DeleteFile((CONST_STRPTR)AMIGA_SSHD_LOG_OLD);
	Rename((CONST_STRPTR)AMIGA_SSHD_LOG, (CONST_STRPTR)AMIGA_SSHD_LOG_OLD);
}

void amiga_log_line(const char *line)
{
	BPTR fh;
	LONG len = (LONG)strlen(line);

	rotate_if_full();
	/* MODE_READWRITE opens an existing file or creates a new one, with a
	 * shared lock (dos.doc, Open). */
	fh = Open((CONST_STRPTR)AMIGA_SSHD_LOG, MODE_READWRITE);
	if (fh == 0)
		return;
	if (Seek(fh, 0, OFFSET_END) != -1) {
		Write(fh, (APTR)line, len);
		Write(fh, (APTR)"\n", 1);
	}
	Close(fh);
}
