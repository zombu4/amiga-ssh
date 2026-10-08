/*
 * amiga_main.h - entry point, listener and session socket of the AmigaOS
 * port.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * port/amiga_main.c holds main(). It prepares the process and calls
 * Dropbear's own main(), which the patches rename to amiga_dropbear_main()
 * on AmigaOS. Dropbear parses the options; in inetd mode (-i) it serves the
 * connection in amiga_session_fd, otherwise it calls amiga_listener().
 */
#ifndef AMIGA_MAIN_H
#define AMIGA_MAIN_H

/* Descriptor of the client connection for inetd mode: the socket handed
 * over by the listener with "-i -S <id>", or 0 when a superserver started
 * the program with the connection on its standard descriptors. */
extern int amiga_session_fd;

/* The session's end of the listener's pre-authentication socket, or -1.
 * Dropbear closes it once the client has logged in (svr-auth.c), which
 * tells the listener that the connection no longer counts as
 * unauthenticated. */
extern int amiga_session_childpipe;

/* Dropbear's main() (svr-main.c), renamed by the patches. */
int amiga_dropbear_main(int argc, char **argv);

/* The listener: accepts connections and starts one session process for
 * each. Called by Dropbear's main() after the options are parsed, when -i
 * was not given. Never returns. */
void amiga_listener(int argc, char **argv);

/* Returns 1 and clears the signal if Ctrl-C (the break signal) is set for
 * this task, 0 otherwise. */
int amiga_break_pending(void);

#endif /* AMIGA_MAIN_H */
