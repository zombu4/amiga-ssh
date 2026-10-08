/*
 * amiga_session.c - shells and commands for SSH channels on AmigaOS.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Implements port/amiga_session.h for the server. Each shell or exec
 * channel gets:
 *  - a connected socket pair (port/amiga_net.c). The server keeps one end
 *    as the channel's descriptor; the other end is handed to the console
 *    handler with ReleaseCopyOfSocket(), because a socket belongs to the
 *    bsdsocket.library base of the task that opened it;
 *  - a control block (struct amiga_child) in public memory, shared with the
 *    handler. Every access to a shared field, here and in the handler,
 *    happens between Forbid() and Permit();
 *  - a console handler process, the program "sshd-console"
 *    (port/amiga_console.c), loaded with LoadSeg() and started with
 *    NP_FreeSeglist, so its code stays valid even if this process exits
 *    first.
 *
 * Lifetime of the block: its reference count starts at 2 (server and
 * handler). amiga_child_free() and the handler's exit each drop one; whoever
 * drops the last one frees it. The handler signals the server only while the
 * block is still attached, so a server that has freed the child, or has
 * exited, is never signalled. An exit handler (atexit) frees every child the
 * server did not free itself, so this holds on every exit path that runs
 * exit().
 */

/* POSIX headers first: proto/bsdsocket.h, pulled in by other port files,
 * defines function-like macros that clash with them; this file itself
 * makes no bsdsocket.library calls. */
#include <sys/types.h>
#include <sys/socket.h>
#include <errno.h>
#include <fcntl.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

#include "amiga_net.h"
#include "amiga_session.h"

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/tasks.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* ---- the control block shared with the handler ------------------------ */

/* One definition for both programs (port/amiga_child.h); the handler checks
 * the first two fields and refuses a block of a different layout. */
#include "amiga_child.h"

/* Stack of the handler process. It keeps its buffers in static data and
 * starts the shell from a separate helper process with its own stack. */
#define HANDLER_STACK 8192

/* Where the handler program is looked for, in this order. */
static const char *const handler_paths[] = {
	"PROGDIR:sshd-console",
	"C:sshd-console",
};

static LONG child_sigbit = -1;		/* "a child finished" signal bit */
static struct amiga_child *children;	/* started and not yet freed */
static int exit_hook_set;

/* ---- helpers ------------------------------------------------------------ */

/* Copies s to dst (which has room) and returns the end of the copy. */
static char *copy_string(char *dst, const char *s, STRPTR *where)
{
	size_t n = strlen(s) + 1;

	memcpy(dst, s, n);
	*where = (STRPTR)dst;
	return dst + n;
}

/* Writes the handler's command line "<id> <address in hex>\n" into buf
 * (at least 32 bytes). */
static void format_arguments(char *buf, long id, ULONG addr)
{
	static const char hex[] = "0123456789abcdef";
	char digits[12];
	unsigned long v;
	int n = 0, i;

	if (id < 0) {
		*buf++ = '-';
		v = (unsigned long)(-(id + 1)) + 1;
	} else {
		v = (unsigned long)id;
	}
	do {
		digits[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v != 0);
	while (n > 0)
		*buf++ = digits[--n];
	*buf++ = ' ';
	for (i = 7; i >= 0; i--)
		*buf++ = hex[(addr >> (i * 4)) & 0xf];
	*buf++ = '\n';
	*buf = '\0';
}

/* Loads the handler program. Requesters are switched off meanwhile, so a
 * missing volume makes LoadSeg() fail instead of waiting for someone at the
 * Amiga to insert it. */
static BPTR load_handler(void)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR oldwin = me->pr_WindowPtr;
	BPTR seg = 0;
	unsigned i;

	me->pr_WindowPtr = (APTR)-1;
	for (i = 0; seg == 0 && i < sizeof(handler_paths) / sizeof(handler_paths[0]); i++)
		seg = LoadSeg((CONST_STRPTR)handler_paths[i]);
	me->pr_WindowPtr = oldwin;
	return seg;
}

static void unlink_child(struct amiga_child *child)
{
	struct amiga_child **pp;

	for (pp = &children; *pp != NULL; pp = &(*pp)->next) {
		if (*pp == child) {
			*pp = child->next;
			child->next = NULL;
			return;
		}
	}
}

/* exit() hook: detaches every child the server did not free, so no handler
 * signals this task after it is gone. */
static void free_all_children(void)
{
	while (children != NULL)
		amiga_child_free(children);
}

/* ---- the interface ------------------------------------------------------ */

int amiga_child_start(const char *command, int pty, int cols, int rows,
		const char *home, const char *const *env, int *fd,
		struct amiga_child **child)
{
	struct amiga_child *blk;
	struct TagItem tags[9];
	size_t size;
	char *p, args[32];
	int sv[2], flags;
	long id;
	BPTR seg;

	if (fd == NULL || child == NULL) {
		errno = EINVAL;
		return -1;
	}

	/* One signal bit per server process, kept for its lifetime. */
	if (child_sigbit < 0) {
		child_sigbit = AllocSignal(-1);
		if (child_sigbit < 0) {
			errno = EAGAIN;
			return -1;
		}
	}
	if (!exit_hook_set) {
		if (atexit(free_all_children) != 0) {
			errno = ENOMEM;
			return -1;
		}
		exit_hook_set = 1;
	}

	/* The block, with its strings stored right behind it, so one
	 * FreeVec() releases everything. */
	size = sizeof(*blk);
	if (command != NULL)
		size += strlen(command) + 1;
	if (home != NULL)
		size += strlen(home) + 1;
	if (env != NULL) {
		const char *const *e;

		for (e = env; *e != NULL; e++)
			size += strlen(*e) + 1;
		size += 1;		/* the empty string that ends the list */
	}
	blk = AllocVec(size, MEMF_PUBLIC | MEMF_CLEAR);
	if (blk == NULL) {
		errno = ENOMEM;
		return -1;
	}
	blk->magic = AMIGA_CHILD_MAGIC;
	blk->size = sizeof(*blk);
	blk->refcount = 2;
	blk->attached = 1;
	blk->server_task = FindTask(NULL);
	blk->server_sigbit = child_sigbit;
	blk->handler_task = NULL;
	blk->handler_sigbit = -1;
	blk->cols = cols;
	blk->rows = rows;
	blk->pty = pty != 0;
	p = (char *)(blk + 1);
	if (command != NULL)
		p = copy_string(p, command, &blk->command);
	if (home != NULL)
		p = copy_string(p, home, &blk->home);
	if (env != NULL) {
		const char *const *e;
		STRPTR first = NULL;

		/* "NAME=VALUE\0NAME=VALUE\0\0"; MEMF_CLEAR already made the
		 * final byte 0. */
		for (e = env; *e != NULL; e++) {
			STRPTR where;

			p = copy_string(p, *e, &where);
			if (first == NULL)
				first = where;
		}
		blk->env = first != NULL ? first : (STRPTR)p;
	}

	/* The channel. sv[0] stays here, non-blocking; sv[1] goes to the
	 * handler. */
	if (socketpair(AF_UNIX, SOCK_STREAM, 0, sv) < 0)
		goto fail_block;
	flags = fcntl(sv[0], F_GETFL, 0);
	if (flags < 0 || fcntl(sv[0], F_SETFL, flags | O_NONBLOCK) < 0)
		goto fail_pair;

	seg = load_handler();
	if (seg == 0) {
		errno = ENOENT;
		goto fail_pair;
	}

	/* Hand the far end to the stack; this process's descriptor for it is
	 * closed at once, the handler takes the socket by its id. */
	id = amiga_socket_to_id(sv[1]);
	if (id < 0)
		goto fail_seg;
	close(sv[1]);
	sv[1] = -1;

	/* NP_Cli: libnix's startup code waits for a Workbench message when
	 * the process has no CLI structure (ncrt0.S), so the handler needs
	 * one. NP_Arguments is copied by CreateNewProc() (dos.doc); the
	 * default NP_Input (NIL:) is kept, as NP_Arguments requires. */
	format_arguments(args, id, (ULONG)blk);
	tags[0].ti_Tag = NP_Seglist;
	tags[0].ti_Data = (ULONG)seg;
	tags[1].ti_Tag = NP_FreeSeglist;
	tags[1].ti_Data = TRUE;
	tags[2].ti_Tag = NP_Arguments;
	tags[2].ti_Data = (ULONG)args;
	tags[3].ti_Tag = NP_Name;
	tags[3].ti_Data = (ULONG)"sshd-console";
	tags[4].ti_Tag = NP_StackSize;
	tags[4].ti_Data = HANDLER_STACK;
	tags[5].ti_Tag = NP_Cli;
	tags[5].ti_Data = TRUE;
	tags[6].ti_Tag = NP_CommandName;
	tags[6].ti_Data = (ULONG)"sshd-console";
	/* No requesters from the handler: nobody may be at the screen. */
	tags[7].ti_Tag = NP_WindowPtr;
	tags[7].ti_Data = (ULONG)-1;
	tags[8].ti_Tag = TAG_END;
	tags[8].ti_Data = 0;
	if (CreateNewProc(tags) == NULL) {
		/* Nothing took the socket: take it back and close it. */
		int s = amiga_socket_from_id(id);

		if (s >= 0)
			close(s);
		errno = ENOMEM;
		goto fail_seg;
	}

	blk->next = children;
	children = blk;
	*fd = sv[0];
	*child = blk;
	return 0;

fail_seg:
	/* CreateNewProc() did not run (or failed): the seglist is still
	 * ours (dos.doc, CreateNewProc RESULT). */
	{
		int saved = errno;

		UnLoadSeg(seg);
		errno = saved;
	}
fail_pair:
	{
		int saved = errno;

		close(sv[0]);
		if (sv[1] >= 0)
			close(sv[1]);
		errno = saved;
	}
fail_block:
	FreeVec(blk);
	return -1;
}

ULONG amiga_child_sigmask(void)
{
	return child_sigbit >= 0 ? 1UL << child_sigbit : 0;
}

int amiga_child_finished(struct amiga_child *child, long *rc)
{
	LONG done, code;

	Forbid();
	done = child->finished;
	code = child->rc;
	Permit();
	if (done && rc != NULL)
		*rc = code;
	return done ? 1 : 0;
}

void amiga_child_winsize(struct amiga_child *child, int cols, int rows)
{
	Forbid();
	child->cols = cols;
	child->rows = rows;
	child->winsize_changed = 1;
	if (child->handler_task != NULL && child->handler_sigbit >= 0)
		Signal(child->handler_task, 1UL << child->handler_sigbit);
	Permit();
}

void amiga_child_break(struct amiga_child *child, ULONG sig)
{
	sig &= SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E
		| SIGBREAKF_CTRL_F;
	if (sig == 0)
		return;
	Forbid();
	child->break_bits |= sig;
	if (child->handler_task != NULL && child->handler_sigbit >= 0)
		Signal(child->handler_task, 1UL << child->handler_sigbit);
	Permit();
}

/* Lets go of the child. The channel descriptor belongs to the caller, who
 * closes it as for any channel; a handler still running sees the server
 * detach, gets CTRL_C for its shell, discards output and exits once the
 * shell or command ends. */
void amiga_child_free(struct amiga_child *child)
{
	int last;

	if (child == NULL)
		return;
	unlink_child(child);
	Forbid();
	child->attached = 0;
	if (!child->finished) {
		child->break_bits |= SIGBREAKF_CTRL_C;
		if (child->handler_task != NULL && child->handler_sigbit >= 0)
			Signal(child->handler_task,
				1UL << child->handler_sigbit);
	}
	last = (--child->refcount == 0);
	Permit();
	if (last)
		FreeVec(child);
}
