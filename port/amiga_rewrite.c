/*
 * amiga_rewrite.c - read a small text file whole and replace it safely.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Everything here uses dos.library and exec.library directly, not libnix's
 * stdio: libnix's read() and write() call __chkabort() (libnix
 * sources/nix/stdio/read.c:23, write.c:22), which on a pending Ctrl-C
 * raises SIGINT, whose default action calls exit(20)
 * (sources/nix/signal/__chkabort.c, raise.c). A program that ends in the
 * middle of a file replacement must not be possible. The dos.library calls used are
 * documented in dos.doc of the NDK 3.2; the one property the design
 * rests on is that Rename() fails if the new name already exists.
 */
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "amiga_rewrite.h"

/* Suffixes of the two helper names next to the file being replaced. */
#define SUFFIX_NEW ".new"
#define SUFFIX_OLD ".old"

void amiga_burn(void *buf, unsigned long len)
{
	volatile unsigned char *v = buf;

	while (len-- > 0)
		*v++ = 0;
}

/* Returns a new buffer holding name followed by suffix, or NULL with
 * IoErr() set to ERROR_NO_FREE_STORE. Free it with FreeVec(). */
static char *name_with(const char *name, const char *suffix)
{
	ULONG nlen = strlen(name), slen = strlen(suffix);
	char *s = AllocVec(nlen + slen + 1, MEMF_ANY);

	if (s == NULL) {
		SetIoErr(ERROR_NO_FREE_STORE);
		return NULL;
	}
	memcpy(s, name, nlen);
	memcpy(s + nlen, suffix, slen + 1);
	return s;
}

/* 1 if an object called name exists, 0 if not. A shared lock is the
 * cheapest way to ask; it is released at once. */
static int exists(const char *name)
{
	BPTR lock = Lock((CONST_STRPTR)name, SHARED_LOCK);

	if (lock == 0)
		return 0;
	UnLock(lock);
	return 1;
}

int amiga_file_read(const char *name, char **data, long *len, int *restored)
{
	struct FileInfoBlock *fib;
	BPTR fh;
	char *buf;
	long size, got = 0, n;

	*data = NULL;
	*len = 0;
	if (restored != NULL)
		*restored = 0;

	fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
	if (fh == 0 && IoErr() == ERROR_OBJECT_NOT_FOUND) {
		/* Interrupted replacement: put the moved-aside original back. */
		char *old = name_with(name, SUFFIX_OLD);

		if (old == NULL)
			return AMIGA_RW_ERROR;
		if (!exists(old)) {
			FreeVec(old);
			SetIoErr(ERROR_OBJECT_NOT_FOUND);
			return AMIGA_RW_MISSING;
		}
		if (!Rename((CONST_STRPTR)old, (CONST_STRPTR)name)) {
			FreeVec(old);
			return AMIGA_RW_ERROR;
		}
		FreeVec(old);
		if (restored != NULL)
			*restored = 1;
		fh = Open((CONST_STRPTR)name, MODE_OLDFILE);
	}
	if (fh == 0)
		return AMIGA_RW_ERROR;

	/* The size from the file system, so one buffer of the right size can
	 * be allocated. */
	fib = AllocDosObject(DOS_FIB, NULL);
	if (fib == NULL) {
		Close(fh);
		SetIoErr(ERROR_NO_FREE_STORE);
		return AMIGA_RW_ERROR;
	}
	if (!ExamineFH(fh, fib)) {
		LONG err = IoErr();

		FreeDosObject(DOS_FIB, fib);
		Close(fh);
		SetIoErr(err);
		return AMIGA_RW_ERROR;
	}
	size = fib->fib_Size;
	FreeDosObject(DOS_FIB, fib);
	if (size < 0 || size > AMIGA_REWRITE_MAX) {
		Close(fh);
		SetIoErr(ERROR_OBJECT_TOO_LARGE);
		return AMIGA_RW_ERROR;
	}

	buf = AllocVec((ULONG)size + 1, MEMF_ANY);
	if (buf == NULL) {
		Close(fh);
		SetIoErr(ERROR_NO_FREE_STORE);
		return AMIGA_RW_ERROR;
	}
	/* Read() may return fewer bytes than asked; 0 is end of file, -1 an
	 * error (dos.doc, Read). */
	while (got < size) {
		n = Read(fh, buf + got, size - got);
		if (n < 0) {
			LONG err = IoErr();

			amiga_burn(buf, (ULONG)got);
			FreeVec(buf);
			Close(fh);
			SetIoErr(err);
			return AMIGA_RW_ERROR;
		}
		if (n == 0)
			break;
		got += n;
	}
	Close(fh);
	buf[got] = '\0';
	*data = buf;
	*len = got;
	return AMIGA_RW_OK;
}

void amiga_file_free(char *data, long len, int wipe)
{
	if (data == NULL)
		return;
	if (wipe)
		amiga_burn(data, (ULONG)len + 1);
	FreeVec(data);
}

/* Writes len bytes to a new file called name. Returns 1, or 0 with IoErr()
 * set; on failure the partly written file is deleted. */
static int write_new(const char *name, const char *data, long len)
{
	BPTR fh = Open((CONST_STRPTR)name, MODE_NEWFILE);
	long done = 0, n;
	LONG err;

	if (fh == 0)
		return 0;
	while (done < len) {
		n = Write(fh, (APTR)(data + done), len - done);
		if (n <= 0) {
			err = IoErr();
			Close(fh);
			DeleteFile((CONST_STRPTR)name);
			SetIoErr(err != 0 ? err : ERROR_DISK_FULL);
			return 0;
		}
		done += n;
	}
	/* Close() writes what is still buffered and can fail doing so
	 * (dos.doc, Close); the file only counts as written if it succeeds. */
	if (!Close(fh)) {
		err = IoErr();
		DeleteFile((CONST_STRPTR)name);
		SetIoErr(err);
		return 0;
	}
	return 1;
}

int amiga_file_replace(const char *name, const char *data, long len)
{
	struct FileInfoBlock *fib = NULL;
	char *tmp = NULL, *old = NULL;
	BPTR lock;
	int had_old = 0, result = AMIGA_RW_ERROR;
	LONG err;

	tmp = name_with(name, SUFFIX_NEW);
	old = name_with(name, SUFFIX_OLD);
	if (tmp == NULL || old == NULL)
		goto out;

	/* Protection bits and comment of the file being replaced, so the new
	 * file looks like the old one. */
	lock = Lock((CONST_STRPTR)name, SHARED_LOCK);
	if (lock != 0) {
		had_old = 1;
		fib = AllocDosObject(DOS_FIB, NULL);
		if (fib == NULL || !Examine(lock, fib)) {
			err = fib == NULL ? ERROR_NO_FREE_STORE : IoErr();
			UnLock(lock);
			SetIoErr(err);
			goto out;
		}
		UnLock(lock);
		if (fib->fib_DirEntryType >= 0) {
			/* A directory of that name: nothing to replace. */
			SetIoErr(ERROR_OBJECT_WRONG_TYPE);
			goto out;
		}
	} else if (IoErr() != ERROR_OBJECT_NOT_FOUND) {
		goto out;
	}

	/* Step 1. MODE_NEWFILE replaces a "name.new" left by a crash. */
	if (!write_new(tmp, data, len))
		goto out;

	/* Step 2. Failing to copy the attributes is not worth losing the
	 * change for; the defaults of a new file are usable. */
	if (had_old) {
		SetProtection((CONST_STRPTR)tmp, fib->fib_Protection);
		if (fib->fib_Comment[0] != '\0')
			SetComment((CONST_STRPTR)tmp, (CONST_STRPTR)fib->fib_Comment);
	}

	if (had_old) {
		/* Step 3. A "name.old" from an earlier run is in the way of
		 * Rename(); it is our own leftover and goes first. */
		if (exists(old) && !DeleteFile((CONST_STRPTR)old)) {
			err = IoErr();
			DeleteFile((CONST_STRPTR)tmp);
			SetIoErr(err);
			goto out;
		}
		if (!Rename((CONST_STRPTR)name, (CONST_STRPTR)old)) {
			err = IoErr();
			DeleteFile((CONST_STRPTR)tmp);
			SetIoErr(err);
			goto out;
		}
	}

	/* Step 4. */
	if (!Rename((CONST_STRPTR)tmp, (CONST_STRPTR)name)) {
		err = IoErr();
		if (had_old)
			Rename((CONST_STRPTR)old, (CONST_STRPTR)name);
		DeleteFile((CONST_STRPTR)tmp);
		SetIoErr(err);
		goto out;
	}

	/* Step 5. */
	result = AMIGA_RW_OK;
	if (had_old && !DeleteFile((CONST_STRPTR)old))
		result = AMIGA_RW_STALE;

out:
	if (fib != NULL)
		FreeDosObject(DOS_FIB, fib);
	if (tmp != NULL)
		FreeVec(tmp);
	if (old != NULL)
		FreeVec(old);
	return result;
}
