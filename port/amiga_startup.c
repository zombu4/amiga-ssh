/*
 * amiga_startup.c - sshd-startup: adds or removes the server's autostart
 * block in a startup script (normally S:User-Startup).
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Usage:
 *   sshd-startup ADD <file>     put the block in (replacing any old one)
 *   sshd-startup REMOVE <file>  take the block out
 *
 * The block is every line from the marker ";BEGIN amiga-ssh" to the marker
 * ";END amiga-ssh", both included. Lines starting with ';' are comments in
 * AmigaDOS scripts, so the markers do nothing when the script runs. A
 * marker is recognised only as a whole line, exactly as written here
 * (trailing spaces, tabs and a CR are allowed).
 *
 * ADD writes the block at the place of the first existing block, or at the
 * end of the file if there is none; every other block is removed, so the
 * file never holds two. If the file does not exist, ADD creates it with
 * just the block. REMOVE deletes every block; a file without one is left
 * untouched. Every line outside the blocks is kept byte for byte.
 *
 * If the markers do not pair up (a BEGIN without END, an END without
 * BEGIN, or a BEGIN inside a block), the file is left untouched and the
 * program fails: removing lines on a guess could cut out part of the
 * user's own startup commands.
 *
 * The file is replaced through a temporary file and Rename()
 * (port/amiga_rewrite.c), so a crash never leaves a half-written startup
 * script, which could stop the machine from booting normally.
 *
 * Why a program and not script commands: AmigaDOS 3.x has no command that
 * deletes a range of lines from a text file. Doing it in a script would
 * need a chain of commands writing intermediate files, each of which can
 * fail half-way; this program does the whole edit in memory and replaces
 * the file in one step.
 *
 * Return codes (dos/dos.h): 0 done (or nothing to do), 5 bad arguments,
 * 20 error (the file is unchanged).
 */
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "amiga_rewrite.h"

/* Version string for the AmigaDOS Version command. */
static const char version[] __attribute__((used)) = "$VER: sshd-startup 1.0 (08.10.2026)";

#define STARTUP_NAME "sshd-startup"

#define MARK_BEGIN ";BEGIN amiga-ssh"
#define MARK_END   ";END amiga-ssh"

/* The block ADD writes. The server needs no Stack command in front of it:
 * it sets up its own stack (see Install-SSH). Output goes to NIL: so the
 * background process holds no console. */
static const char block[] =
	MARK_BEGIN "\n"
	"; SSH server (amiga-ssh). Added by Install-SSH, removed by\n"
	"; Uninstall-SSH; edit outside these marker lines only.\n"
	"Run >NIL: C:sshd\n"
	MARK_END "\n";

/* What a line is, for the block parser. */
#define LINE_TEXT  0
#define LINE_BEGIN 1
#define LINE_END   2

/* Writes a string to the program's output. */
static void say(const char *s)
{
	Write(Output(), (APTR)s, (LONG)strlen(s));
}

/* Prints "sshd-startup: <what>: <DOS error text>". */
static void fail(LONG err, const char *what, const char *file)
{
	char header[160];
	size_t plen = strlen(STARTUP_NAME), wlen = strlen(what), flen = strlen(file);

	if (plen + 2 + wlen + 1 + flen + 1 > sizeof(header)) {
		flen = 0;
		if (plen + 2 + wlen + 2 > sizeof(header))
			wlen = sizeof(header) - plen - 4;
	}
	memcpy(header, STARTUP_NAME, plen);
	memcpy(header + plen, ": ", 2);
	memcpy(header + plen + 2, what, wlen);
	header[plen + 2 + wlen] = ' ';
	memcpy(header + plen + 3 + wlen, file, flen);
	header[plen + 3 + wlen + flen] = '\0';
	PrintFault(err, (CONST_STRPTR)header);
}

/* ASCII case-insensitive comparison of two NUL-terminated strings, for the
 * keywords ADD and REMOVE (AmigaDOS keywords are case-insensitive). */
static int same_word(const char *a, const char *b)
{
	for (;; a++, b++) {
		char ca = *a, cb = *b;

		if (ca >= 'a' && ca <= 'z')
			ca -= 'a' - 'A';
		if (cb >= 'a' && cb <= 'z')
			cb -= 'a' - 'A';
		if (ca != cb)
			return 0;
		if (ca == '\0')
			return 1;
	}
}

/* Classifies the line text[0..len) (without its LF). */
static int line_kind(const char *text, long len)
{
	/* Ignore trailing blanks and a CR from a file edited elsewhere. */
	while (len > 0 && (text[len - 1] == ' ' || text[len - 1] == '\t'
			   || text[len - 1] == '\r'))
		len--;
	if (len == (long)strlen(MARK_BEGIN)
	    && memcmp(text, MARK_BEGIN, (size_t)len) == 0)
		return LINE_BEGIN;
	if (len == (long)strlen(MARK_END)
	    && memcmp(text, MARK_END, (size_t)len) == 0)
		return LINE_END;
	return LINE_TEXT;
}

/* Builds the new file text from old (oldlen bytes, may be 0): every line
 * outside the marked blocks is copied; with add nonzero, the block is
 * written where the first old block was, or appended. Stores the result
 * (a buffer from AllocVec()) in *out and its length in *outlen, and in
 * *blocks how many old blocks were found.
 * Returns 0, -1 if the markers do not pair up (*bad_line holds the
 * 1-based line number), or -2 if memory ran out. */
static int rebuild(const char *old, long oldlen, int add, char **out,
		long *outlen, int *blocks, long *bad_line)
{
	long blen = (long)strlen(block);
	long cap = oldlen + 1 + blen + 1;
	long pos = 0, i = 0, lineno = 0;
	int inside = 0, placed = 0;
	char *buf;

	*out = NULL;
	*outlen = 0;
	*blocks = 0;
	*bad_line = 0;
	buf = AllocVec((ULONG)cap, MEMF_ANY);
	if (buf == NULL)
		return -2;

	while (i < oldlen) {
		long start = i, end;
		int kind;

		while (i < oldlen && old[i] != '\n')
			i++;
		end = i;
		if (i < oldlen)
			i++;		/* past the LF */
		lineno++;
		kind = line_kind(old + start, end - start);

		if (kind == LINE_BEGIN) {
			if (inside)
				goto unpaired;
			inside = 1;
			(*blocks)++;
			/* The new block goes where the first old one was. */
			if (add && !placed) {
				memcpy(buf + pos, block, (size_t)blen);
				pos += blen;
				placed = 1;
			}
			continue;
		}
		if (kind == LINE_END) {
			if (!inside)
				goto unpaired;
			inside = 0;
			continue;
		}
		if (inside)
			continue;	/* a line of an old block */
		memcpy(buf + pos, old + start, (size_t)(i - start));
		pos += i - start;
	}
	if (inside) {
		lineno = 0;	/* reported as "end of file" */
		goto unpaired;
	}
	if (add && !placed) {
		if (pos > 0 && buf[pos - 1] != '\n')
			buf[pos++] = '\n';
		memcpy(buf + pos, block, (size_t)blen);
		pos += blen;
	}
	*out = buf;
	*outlen = pos;
	return 0;

unpaired:
	FreeVec(buf);
	*bad_line = lineno;
	return -1;
}

/* Writes the decimal value of n (n >= 0) into buf (at least 12 bytes). */
static void put_number(char *buf, long n)
{
	char tmp[12];
	int len = 0, i;

	do {
		tmp[len++] = (char)('0' + n % 10);
		n /= 10;
	} while (n > 0 && len < (int)sizeof(tmp));
	for (i = 0; i < len; i++)
		buf[i] = tmp[len - 1 - i];
	buf[len] = '\0';
}

int main(int argc, char **argv)
{
	const char *file;
	char *old = NULL, *text = NULL;
	long oldlen = 0, textlen = 0, bad_line = 0;
	int add, restored = 0, blocks = 0, r;

	/* Two arguments: the action and the file. */
	if (argc != 3 || (!same_word(argv[1], "ADD")
			  && !same_word(argv[1], "REMOVE"))) {
		say("Usage: " STARTUP_NAME " ADD <file>     add the amiga-ssh block\n"
		    "       " STARTUP_NAME " REMOVE <file>  remove it\n");
		return RETURN_WARN;
	}
	add = same_word(argv[1], "ADD");
	file = argv[2];

	r = amiga_file_read(file, &old, &oldlen, &restored);
	if (r == AMIGA_RW_ERROR) {
		fail(IoErr(), "cannot read", file);
		return RETURN_FAIL;
	}
	if (restored) {
		say(STARTUP_NAME ": restored ");
		say(file);
		say(" from its .old copy (an earlier change was interrupted)\n");
	}
	if (r == AMIGA_RW_MISSING && !add) {
		/* Nothing to remove from a file that is not there. */
		say(STARTUP_NAME ": ");
		say(file);
		say(" does not exist; nothing to remove\n");
		return RETURN_OK;
	}

	r = rebuild(old, oldlen, add, &text, &textlen, &blocks, &bad_line);
	if (r == -2) {
		amiga_file_free(old, oldlen, 0);
		fail(ERROR_NO_FREE_STORE, "cannot edit", file);
		return RETURN_FAIL;
	}
	if (r == -1) {
		char num[12];

		amiga_file_free(old, oldlen, 0);
		say(STARTUP_NAME ": the marker lines \"" MARK_BEGIN "\" and \""
			MARK_END "\" in ");
		say(file);
		say(" do not pair up (at ");
		if (bad_line > 0) {
			put_number(num, bad_line);
			say("line ");
			say(num);
		} else {
			say("the end of the file");
		}
		say("). The file was not changed; please fix it with a text "
		    "editor.\n");
		return RETURN_FAIL;
	}

	/* REMOVE with no block, or ADD with exactly the wanted text already
	 * there: leave the file alone. */
	if ((!add && blocks == 0)
	    || (textlen == oldlen && old != NULL
		&& memcmp(text, old, (size_t)oldlen) == 0)) {
		amiga_file_free(old, oldlen, 0);
		FreeVec(text);
		say(STARTUP_NAME ": ");
		say(file);
		say(add ? " already has the amiga-ssh block\n"
			: " has no amiga-ssh block; nothing to remove\n");
		return RETURN_OK;
	}
	amiga_file_free(old, oldlen, 0);

	r = amiga_file_replace(file, text, textlen);
	FreeVec(text);
	if (r == AMIGA_RW_ERROR) {
		fail(IoErr(), "cannot write", file);
		return RETURN_FAIL;
	}
	say(STARTUP_NAME ": ");
	say(add ? "added the amiga-ssh block to " : "removed the amiga-ssh block from ");
	say(file);
	say("\n");
	if (r == AMIGA_RW_STALE) {
		say(STARTUP_NAME ": note: could not delete ");
		say(file);
		say(".old (the previous version); it can be deleted by hand\n");
	}
	return RETURN_OK;
}
