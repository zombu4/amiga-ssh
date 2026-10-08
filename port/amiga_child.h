/*
 * amiga_child.h - the control block shared by the server and sshd-console.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Private to port/amiga_session.c (the server side) and port/amiga_console.c
 * (the console handler). The server allocates one block per channel in
 * public memory and passes its address to the handler on the command line;
 * both programs are built from this one definition. The handler checks
 * magic and size before it touches anything, so a block from a different
 * build is refused unchanged.
 */
#ifndef AMIGA_CHILD_H
#define AMIGA_CHILD_H

#include <exec/types.h>
#include <exec/tasks.h>

#define AMIGA_CHILD_MAGIC 0x53534843UL	/* "SSHC" */

struct amiga_child {
	ULONG magic;			/* AMIGA_CHILD_MAGIC */
	ULONG size;			/* sizeof(struct amiga_child) */

	/* Shared; read and written only under Forbid(). */
	LONG refcount;			/* server + handler; 0 frees the block */
	LONG attached;			/* 0 once the server called free */
	struct Task *server_task;	/* signalled when the child finishes */
	LONG server_sigbit;		/* its signal bit */
	struct Task *handler_task;	/* the handler, NULL before/after */
	LONG handler_sigbit;		/* control signal bit of the handler */
	LONG finished;			/* 1 once rc is valid */
	LONG rc;			/* return code of the shell/command */
	ULONG break_bits;		/* SIGBREAKF_CTRL_x requested by server */
	LONG winsize_changed;		/* set by the server on window-change */
	LONG cols, rows;		/* terminal size from the client */

	/* Set by the server before the handler starts; constant after. */
	LONG pty;			/* nonzero: client asked for a terminal */
	STRPTR command;			/* command line, NULL for a shell */
	STRPTR home;			/* current directory, NULL: unchanged */
	STRPTR env;			/* "NAME=VALUE" strings, each ended by a
					 * NUL, the list by an empty one; NULL:
					 * none */

	/* Server-private: list of children not yet freed. */
	struct amiga_child *next;
};

#endif /* AMIGA_CHILD_H */
