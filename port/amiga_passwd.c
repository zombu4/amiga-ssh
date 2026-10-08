/*
 * amiga_passwd.c - sshpasswd: sets the SSH login password.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Usage:
 *   sshpasswd             asks for the new password twice, without echo
 *   sshpasswd <password>  sets it without asking (used by Install-SSH)
 *
 * The password is stored only as a SHA-256 crypt hash ("$5$<salt>$<hash>",
 * port/amiga_crypt.c) on the "password=" line of the server's config file
 * (AMIGA_SSHD_CONFIG, format in port/amiga_user.c). Every other line of the
 * file is kept byte for byte. The file is replaced through a temporary file
 * and Rename() (port/amiga_rewrite.c), so a crash or power loss never
 * leaves a half-written config.
 *
 * The salt is 16 characters made from a SHA-256 digest over fresh samples
 * from amiga_entropy() (port/amiga_random.c) and the server's seed file
 * (AMIGA_SSHD_SEED), if there is one. The seed file is only read.
 *
 * Return codes (dos/dos.h): 0 (RETURN_OK) password set; 5 (RETURN_WARN)
 * nothing changed because the two entries differed, the password was empty
 * or the user cancelled; 20 (RETURN_FAIL) error, nothing changed.
 *
 * All console and file I/O goes through dos.library, not libnix's stdio:
 * libnix's read() and write() call __chkabort() (libnix
 * sources/nix/stdio/read.c:23, write.c:22), which on a pending Ctrl-C
 * raises SIGINT, whose default action prints "***Break" and calls exit(20)
 * (sources/nix/signal/__chkabort.c, raise.c). That would end the program
 * with the console still in raw mode. Here Ctrl-C is handled by this
 * program, which always switches the console back first.
 */
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "tomcrypt.h"

#include "amiga_crypt.h"
#include "amiga_paths.h"
#include "amiga_random.h"
#include "amiga_rewrite.h"

/* Version string for the AmigaDOS Version command. */
static const char version[] __attribute__((used)) = "$VER: sshpasswd 1.0 (08.10.2026)";

#define PASSWD_NAME "sshpasswd"

/* Longest password accepted from the keyboard. Room for the NUL follows. */
#define PASS_MAX 127

/* The key on the config line that holds the hash, as amiga_user.c reads it
 * (column 0, case-sensitive). */
#define PASS_KEY "password="
#define PASS_KEY_LEN 9

/* Written when the config file does not exist yet (the installer's
 * defaults). */
#define NEW_CONFIG_HEAD "user=amiga\nhome=SYS:\n"

/* At most this much of the seed file is mixed into the salt. The file the
 * server writes is far smaller; the limit only guards against a wrong file
 * of that name. */
#define SEED_READ_MAX (64L * 1024L)

/* How long one WaitForChar() waits, in microseconds, before Ctrl-C is
 * checked again. */
#define KEY_POLL_US 100000L

/* Results of read_password(). */
#define READ_OK      0
#define READ_CANCEL  1	/* Ctrl-C (signal or key) */
#define READ_ERROR   2	/* the console failed or reached end of file */

/* Control characters the line editor knows. */
#define KEY_CTRL_C 0x03
#define KEY_BS     0x08
#define KEY_LF     0x0a
#define KEY_CR     0x0d
#define KEY_CTRL_X 0x18
#define KEY_ESC    0x1b
#define KEY_DEL    0x7f
#define KEY_CSI    0x9b

/* ---- output --------------------------------------------------------- */

/* Writes a string to the program's output. */
static void say(const char *s)
{
	Write(Output(), (APTR)s, (LONG)strlen(s));
}

/* Prints "sshpasswd: <what>: <DOS error text>" for the DOS error code err
 * (PrintFault() writes to the program's error stream as set up by DOS). */
static void fail(LONG err, const char *what)
{
	char header[96];
	size_t plen = strlen(PASSWD_NAME), wlen = strlen(what);

	if (plen + 2 + wlen + 1 > sizeof(header))
		wlen = sizeof(header) - plen - 3;
	memcpy(header, PASSWD_NAME, plen);
	memcpy(header + plen, ": ", 2);
	memcpy(header + plen + 2, what, wlen);
	header[plen + 2 + wlen] = '\0';
	PrintFault(err, (CONST_STRPTR)header);
}

/* ---- reading a password without echo -------------------------------- */

/* Reads one line from the console in raw mode into buf (size bytes, NUL
 * terminated), echoing nothing. The console is in raw mode only inside
 * this function and is switched back on every way out.
 *
 * Keys: Return or Enter ends the line; Backspace and Del remove the last
 * character; Ctrl-X clears the line; Ctrl-C cancels. In raw mode the Amiga
 * console sends cursor and function keys as escape sequences (ESC [ ... or
 * the single-byte CSI 0x9B, ended by a byte from 0x40 to 0x7E); these are
 * read and ignored, so a stray arrow key does not end up in the password.
 * Other control bytes are ignored as well.
 *
 * Ctrl-C is honoured both as the byte 0x03 and as the break signal, so it
 * works whichever of the two the console delivers in raw mode. The signal
 * is polled between WaitForChar() calls, which return after KEY_POLL_US
 * even when no key is pressed. */
static int read_password(BPTR in, char *buf, size_t size)
{
	size_t len = 0;
	int in_seq = 0;		/* inside an escape sequence */
	int esc = 0;		/* the last byte was a lone ESC */
	int result = READ_ERROR;
	UBYTE c;

	if (!SetMode(in, 1)) {
		fail(IoErr(), "cannot switch the console to raw mode");
		return READ_ERROR;
	}
	for (;;) {
		if (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) {
			result = READ_CANCEL;
			break;
		}
		if (!WaitForChar(in, KEY_POLL_US))
			continue;
		if (Read(in, &c, 1) != 1)
			break;	/* end of input or error: READ_ERROR */

		if (in_seq) {
			/* Skip parameter bytes up to the final byte. */
			if (c >= 0x40 && c <= 0x7e)
				in_seq = 0;
			continue;
		}
		if (esc) {
			esc = 0;
			if (c == '[') {
				in_seq = 1;
				continue;
			}
			/* ESC followed by anything else: drop both. */
			continue;
		}
		if (c == KEY_CSI) {
			in_seq = 1;
			continue;
		}
		if (c == KEY_ESC) {
			esc = 1;
			continue;
		}
		if (c == KEY_CTRL_C) {
			result = READ_CANCEL;
			break;
		}
		if (c == KEY_CR || c == KEY_LF) {
			result = READ_OK;
			break;
		}
		if (c == KEY_BS || c == KEY_DEL) {
			if (len > 0)
				buf[--len] = '\0';
			continue;
		}
		if (c == KEY_CTRL_X) {
			amiga_burn(buf, len);
			len = 0;
			continue;
		}
		/* Printable ASCII and ISO-8859-1 (0xA0-0xFF) only; 0x80-0x9F
		 * are control codes in the Amiga character set. */
		if (c < 0x20 || (c >= 0x80 && c < 0xa0))
			continue;
		if (len + 1 < size)
			buf[len++] = (char)c;
	}
	buf[len] = '\0';
	c = 0;
	SetMode(in, 0);
	/* Return was not echoed in raw mode; end the prompt line. */
	say("\n");
	if (result == READ_ERROR)
		fail(IoErr(), "cannot read from the console");
	return result;
}

/* ---- salt ----------------------------------------------------------- */

/* Mixes up to SEED_READ_MAX bytes of the seed file into hs. A missing or
 * unreadable seed file is not an error: the entropy samples are used on
 * their own then. */
static void add_seed_file(hash_state *hs)
{
	unsigned char chunk[256];
	BPTR fh = Open((CONST_STRPTR)AMIGA_SSHD_SEED, MODE_OLDFILE);
	long total = 0, n;

	if (fh == 0)
		return;
	while (total < SEED_READ_MAX) {
		n = Read(fh, chunk, sizeof(chunk));
		if (n <= 0)
			break;
		sha256_process(hs, chunk, (unsigned long)n);
		total += n;
	}
	Close(fh);
	amiga_burn(chunk, sizeof(chunk));
}

/* Makes a fresh "$5$<16 salt characters>" setting for amiga_sha256_crypt().
 * setting must hold 3 + AMIGA_SALT_LEN + 1 bytes. */
static void make_setting(char *setting)
{
	unsigned char pool[AMIGA_ENTROPY_BYTES];
	unsigned char digest[32];
	char salt[AMIGA_SALT_LEN + 1];
	hash_state hs;
	size_t n;

	sha256_init(&hs);
	n = amiga_entropy(pool, sizeof(pool));
	sha256_process(&hs, pool, (unsigned long)n);
	add_seed_file(&hs);
	sha256_done(&hs, digest);

	/* amiga_make_salt() takes AMIGA_SALT_LEN (16) of the 32 digest bytes
	 * and maps each to one of crypt's 64 characters by its low 6 bits. */
	amiga_make_salt(salt, digest);
	memcpy(setting, "$5$", 3);
	memcpy(setting + 3, salt, AMIGA_SALT_LEN + 1);

	amiga_burn(pool, sizeof(pool));
	amiga_burn(digest, sizeof(digest));
	amiga_burn(&hs, sizeof(hs));
}

/* ---- the config file ------------------------------------------------- */

/* Builds the new config text from the old one (old may be NULL: no file)
 * with the line "password=<hash>". Returns a buffer from AllocVec() and
 * its length in *outlen, or NULL if memory ran out.
 *
 * The first "password=" line is replaced in place; any further ones are
 * dropped. amiga_user.c keeps the value of the last "password=" line it
 * reads, so a second, older line left in the file would still be the one
 * that counts. Without such a line, one is appended (after a line break if
 * the file does not end with one). */
static char *build_config(const char *old, long oldlen, const char *hash,
		long *outlen)
{
	long hlen = (long)strlen(hash);
	long linelen = PASS_KEY_LEN + hlen + 1;	/* key, hash, LF */
	long cap, pos = 0, i = 0;
	int replaced = 0;
	char *out;

	if (old == NULL) {
		old = NEW_CONFIG_HEAD;
		oldlen = (long)strlen(NEW_CONFIG_HEAD);
	}
	/* The result is never longer than the old text, one extra line
	 * break and the new line. */
	cap = oldlen + 1 + linelen + 1;
	out = AllocVec((ULONG)cap, MEMF_ANY);
	if (out == NULL)
		return NULL;

	while (i < oldlen) {
		long start = i, end;

		while (i < oldlen && old[i] != '\n')
			i++;
		end = i;		/* the line without its LF */
		if (i < oldlen)
			i++;		/* past the LF */

		if (end - start >= PASS_KEY_LEN
		    && memcmp(old + start, PASS_KEY, PASS_KEY_LEN) == 0) {
			if (!replaced) {
				memcpy(out + pos, PASS_KEY, PASS_KEY_LEN);
				pos += PASS_KEY_LEN;
				memcpy(out + pos, hash, (size_t)hlen);
				pos += hlen;
				out[pos++] = '\n';
				replaced = 1;
			}
			continue;
		}
		/* Any other line, with its LF if it had one. */
		memcpy(out + pos, old + start, (size_t)(i - start));
		pos += i - start;
	}
	if (!replaced) {
		if (pos > 0 && out[pos - 1] != '\n')
			out[pos++] = '\n';
		memcpy(out + pos, PASS_KEY, PASS_KEY_LEN);
		pos += PASS_KEY_LEN;
		memcpy(out + pos, hash, (size_t)hlen);
		pos += hlen;
		out[pos++] = '\n';
	}
	*outlen = pos;
	return out;
}

/* Hashes password and writes it to the config file. Returns a return
 * code for the program. */
static int set_password(const char *password)
{
	char setting[3 + AMIGA_SALT_LEN + 1];
	char hash[128];
	char *old = NULL, *text;
	long oldlen = 0, textlen = 0;
	int restored = 0, r;

	/* Read the config first: a file that cannot be read must stop us
	 * before anything is computed or written. */
	r = amiga_file_read(AMIGA_SSHD_CONFIG, &old, &oldlen, &restored);
	if (r == AMIGA_RW_ERROR) {
		fail(IoErr(), "cannot read " AMIGA_SSHD_CONFIG);
		return RETURN_FAIL;
	}
	if (restored)
		say(PASSWD_NAME ": restored " AMIGA_SSHD_CONFIG " from "
			AMIGA_SSHD_CONFIG ".old (an earlier change was interrupted)\n");

	make_setting(setting);
	if (amiga_sha256_crypt(password, setting, hash, sizeof(hash)) == NULL) {
		/* Only possible if the buffer were too small or the setting
		 * not "$5$"; neither is the case, but the result is checked. */
		amiga_file_free(old, oldlen, 1);
		amiga_burn(setting, sizeof(setting));
		say(PASSWD_NAME ": hashing the password failed\n");
		return RETURN_FAIL;
	}
	amiga_burn(setting, sizeof(setting));

	text = build_config(old, oldlen, hash, &textlen);
	amiga_file_free(old, oldlen, 1);
	amiga_burn(hash, sizeof(hash));
	if (text == NULL) {
		fail(ERROR_NO_FREE_STORE, "cannot build the new config");
		return RETURN_FAIL;
	}

	r = amiga_file_replace(AMIGA_SSHD_CONFIG, text, textlen);
	if (r == AMIGA_RW_ERROR) {
		LONG err = IoErr();

		amiga_burn(text, (ULONG)textlen);
		FreeVec(text);
		fail(err, "cannot write " AMIGA_SSHD_CONFIG);
		return RETURN_FAIL;
	}
	amiga_burn(text, (ULONG)textlen);
	FreeVec(text);
	if (r == AMIGA_RW_STALE)
		say(PASSWD_NAME ": note: could not delete " AMIGA_SSHD_CONFIG
			".old, which holds the previous password hash; "
			"delete it by hand\n");
	say("Password changed.\n");
	return RETURN_OK;
}

/* ---- main ----------------------------------------------------------- */

/* The interactive way: two entries without echo, which must match. */
static int ask_password(void)
{
	char first[PASS_MAX + 1], second[PASS_MAX + 1];
	BPTR in = Input();
	int r, rc;

	if (in == 0 || !IsInteractive(in)) {
		say(PASSWD_NAME ": input is not a console; give the password as "
			"an argument instead: " PASSWD_NAME " <password>\n");
		return RETURN_FAIL;
	}

	second[0] = '\0';
	say("New password: ");
	r = read_password(in, first, sizeof(first));
	/* An empty first entry is refused below without asking again. */
	if (r == READ_OK && first[0] != '\0') {
		say("Retype new password: ");
		r = read_password(in, second, sizeof(second));
	}

	if (r == READ_CANCEL) {
		say("Cancelled; the password was not changed.\n");
		rc = RETURN_WARN;
	} else if (r == READ_ERROR) {
		rc = RETURN_FAIL;
	} else if (first[0] == '\0') {
		say("The password must not be empty; it was not changed.\n");
		rc = RETURN_WARN;
	} else if (strcmp(first, second) != 0) {
		say("The passwords differ; the password was not changed.\n");
		rc = RETURN_WARN;
	} else {
		rc = set_password(first);
	}
	amiga_burn(first, sizeof(first));
	amiga_burn(second, sizeof(second));
	return rc;
}

int main(int argc, char **argv)
{
	int rc;

	/* argc is 0 when started from Workbench; libnix then opens a console
	 * window as Input() and Output() (libnix nix20/misc/__nocommandline.c,
	 * __stdiowin), so the interactive way works there too. */
	if (argc <= 1)
		return ask_password();

	if (argc > 2 || strcmp(argv[1], "?") == 0) {
		say("Usage: " PASSWD_NAME "             (asks for the password)\n"
		    "       " PASSWD_NAME " <password>  (sets it directly)\n");
		return RETURN_FAIL;
	}
	if (argv[1][0] == '\0') {
		say("The password must not be empty; it was not changed.\n");
		return RETURN_WARN;
	}
	rc = set_password(argv[1]);
	/* argv[1] points into libnix's private copy of the command line
	 * (__nocommandline.c), so it can be cleared here. */
	amiga_burn(argv[1], strlen(argv[1]));
	return rc;
}
