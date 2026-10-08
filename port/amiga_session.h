/*
 * amiga_session.h - shells and commands for SSH channels on AmigaOS.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Replaces fork()/exec()/pipes/pty for Dropbear's session channels. For each
 * "shell" or "exec" request the server calls amiga_child_start(). It returns
 * one socket descriptor for the channel (readfd = writefd, as upstream's pty
 * master); behind it a console handler process serves the AmigaDOS shell or
 * command (see docs/DESIGN.md, Sessions). When the shell or command ends,
 * the handler signals the server task with the bit in amiga_child_sigmask();
 * the server then calls amiga_child_finished() to collect return codes.
 */
#ifndef AMIGA_SESSION_H
#define AMIGA_SESSION_H

#include <exec/types.h>

struct amiga_child;

/* Starts a console handler and, through it, either an interactive shell
 * (command == NULL: "NewShell *") or one AmigaDOS command line.
 *  pty   - nonzero if the client requested a terminal (pty-req): the
 *          handler then echoes and edits lines itself, turns Ctrl-C/D/E/F
 *          into break signals and converts LF to CR LF on output; zero
 *          passes bytes through unchanged.
 *  cols, rows - terminal size from pty-req (0 if unknown).
 *  home  - current directory for the shell or command (NULL: unchanged).
 * On success stores the channel descriptor in *fd (a connected socket,
 * already non-blocking), the child in *child, and returns 0. On failure
 * returns -1 with errno set and nothing left running. */
int amiga_child_start(const char *command, int pty, int cols, int rows,
		const char *home, int *fd, struct amiga_child **child);

/* The Amiga signal mask the server must add to its WaitSelect() wait; set
 * when any child has finished. 0 before the first amiga_child_start(). */
ULONG amiga_child_sigmask(void);

/* Returns 1 and the child's AmigaDOS return code in *rc if child has
 * finished, 0 if it is still running. Never blocks. */
int amiga_child_finished(struct amiga_child *child, long *rc);

/* Records a new terminal size (window-change request). */
void amiga_child_winsize(struct amiga_child *child, int cols, int rows);

/* Sends a break signal to the running shell or command: sig is one of
 * SIGBREAKF_CTRL_C/D/E/F (SSH "signal" request INT maps to CTRL_C). */
void amiga_child_break(struct amiga_child *child, ULONG sig);

/* Lets go of the child, whether or not it has finished; never blocks. If it
 * is still running, it gets CTRL_C and its handler frees the shared block
 * once the shell or command ends. The channel descriptor from
 * amiga_child_start() is not closed here: it belongs to the caller (Dropbear
 * closes it with the channel). */
void amiga_child_free(struct amiga_child *child);

#endif /* AMIGA_SESSION_H */
