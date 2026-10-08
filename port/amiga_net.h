/*
 * amiga_net.h - socket layer of the AmigaOS port.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * libnix maps bsdsocket.library sockets onto POSIX file descriptors, but its
 * select(), fcntl() and socket close are unusable for a server (see
 * docs/DESIGN.md). This layer replaces them. Every socket descriptor the
 * server uses is created here, so all of them share one set of descriptor
 * routines.
 */
#ifndef AMIGA_NET_H
#define AMIGA_NET_H

#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <exec/types.h>

/* select() that also returns when one of the Amiga signals in *sigmask
 * arrives. On return *sigmask holds the signals that arrived (0 if none).
 * sigmask may be NULL, which makes this exactly select(). */
int amiga_waitselect(int nfds, fd_set *readfds, fd_set *writefds,
		fd_set *exceptfds, struct timeval *timeout, ULONG *sigmask);

/* socketpair() for AF_UNIX (or AF_INET) stream sockets, made from two
 * connected 127.0.0.1 TCP sockets. The NDK headers do not declare it. */
int socketpair(int domain, int type, int protocol, int sv[2]);

/* Hands the socket behind descriptor fd to the network stack so another
 * process can take it with amiga_socket_from_id(). The descriptor stays open
 * in this process. Returns the id, or -1 with errno set. */
long amiga_socket_to_id(int fd);

/* Takes a socket handed over with amiga_socket_to_id() (or by a
 * superserver) and returns a new descriptor for it, or -1 with errno set. */
int amiga_socket_from_id(long id);

#endif /* AMIGA_NET_H */
