/*
 * amiga_net.c - socket layer of the AmigaOS port.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * libnix (as shipped with the m68k-amigaos-gcc toolchain) gives every
 * bsdsocket.library socket a POSIX file descriptor. Three of its routines
 * cannot be used by a server:
 *
 *  - select() never opens timer.device but sends it a timer request for any
 *    call with a timeout, and passes WaitSelect() the highest socket number
 *    instead of that number plus one.
 *  - fcntl() returns -1 for every call without setting errno.
 *  - closing a socket descriptor closes the socket but never frees the
 *    descriptor slot.
 *
 * This file replaces select(), fcntl(), socket() and accept() and adds
 * socketpair() (libnix's only returns ENOSYS). Every socket descriptor
 * created here uses the descriptor routines below, whose close frees the
 * slot. libnix's other socket calls (bind, connect, send, recv, ...) look up
 * the real socket through the descriptor and keep working unchanged.
 *
 * Library calls are made through the inline macros of proto/bsdsocket.h.
 * Where this file defines a POSIX function of the same name, the name is
 * written in parentheses, e.g. "int (socket)(...)", so the function-like
 * macro is not expanded in the definition.
 */
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <errno.h>
#include <fcntl.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>

#include "amiga_net.h"

/* Library headers last: proto/bsdsocket.h defines function-like macros
 * (gethostid, getdtablesize, ...) that would break the POSIX prototypes in
 * the headers above. */
#include <exec/types.h>
#include <proto/exec.h>
#include <libraries/bsdsocket.h>
#include <proto/bsdsocket.h>

/* libnix internals, exported by libnix.a and libsocket.a. The descriptor
 * table itself is public (stdio.h: StdFileDes, _lx_fhfromfd, LX_SOCKET). */
extern StdFileDes *__allocfd(void);

/* libnix's internal select command and mode values
 * (sources/headers/select.h), used by the lx_select hook below. */
#define SELCMD_PREPARE 0
#define SELCMD_CHECK   1
#define SELCMD_POLL    2
#define SELMODE_IN     0
#define SELMODE_OUT    1
#define SELMODE_EXC    2

/* O_NONBLOCK state of each socket descriptor. libnix's descriptor has no
 * spare field for it, and bsdsocket.library cannot report FIONBIO, so it is
 * kept here. The socket number is stored with the flag: a descriptor that
 * was reused for another socket reads as blocking, which is what a new
 * socket is. Socket descriptors are refused at FD_SETSIZE and above (they
 * could not be passed to select() either), so the table covers them all. */
static struct {
	long sock;
	unsigned char nonblock;
} nbstate[FD_SETSIZE];

static void nb_set(int fd, long sock, int on)
{
	nbstate[fd].sock = sock;
	nbstate[fd].nonblock = on ? 1 : 0;
}

static int nb_get(int fd, long sock)
{
	return nbstate[fd].sock == sock && nbstate[fd].nonblock;
}

/* Returns the descriptor's socket number, or -1 with errno EBADF if fd is
 * not an open socket descriptor. */
static long sock_of(int fd)
{
	StdFileDes *sfd = _lx_fhfromfd(fd);

	if (sfd == NULL || !(sfd->lx_flags & LX_SOCKET)) {
		errno = EBADF;
		return -1;
	}
	return sfd->lx_sock;
}

/* ---- descriptor routines for every socket created here ---------------- */

static int wrap_socket(long sock, int family, int type, int protocol);

static ssize_t __stdargs net_read(StdFileDes *sfd, void *buf, size_t len)
{
	return recv(sfd->lx_sock, buf, len, 0);
}

static ssize_t __stdargs net_write(StdFileDes *sfd, const void *buf, size_t len)
{
	return send(sfd->lx_sock, (APTR)buf, len, 0);
}

static int __stdargs net_close(StdFileDes *sfd)
{
	long rc = CloseSocket(sfd->lx_sock);

	if ((unsigned)sfd->lx_pos < FD_SETSIZE)
		nb_set(sfd->lx_pos, -1, 0);
	sfd->lx_inuse = 0;	/* free the slot; libnix's socket close does not */
	return rc < 0 ? -1 : 0;
}

/* dup(): a second, independent reference to the same connection, made the
 * way bsdsocket.library shares a socket between owners. */
static int __stdargs net_dup(StdFileDes *sfd)
{
	long id, sock;

	id = ReleaseCopyOfSocket(sfd->lx_sock, UNIQUE_ID);
	if (id < 0)
		return -1;
	sock = ObtainSocket(id, sfd->lx_family, sfd->lx_domain, sfd->lx_protocol);
	if (sock < 0)
		return -1;
	return wrap_socket(sock, sfd->lx_family, sfd->lx_domain, sfd->lx_protocol);
}

static int __stdargs net_fstat(StdFileDes *sfd, struct stat *sb)
{
	LONG value = 0;
	socklen_t size = sizeof(value);

	memset(sb, 0, sizeof(*sb));
#ifdef S_IFSOCK
	sb->st_mode = S_IFSOCK | 0666;
#endif
	if (getsockopt(sfd->lx_sock, SOL_SOCKET, SO_SNDBUF, &value, &size) == 0)
		sb->st_blksize = value;
	return 0;
}

/* libnix's select() calls this hook per descriptor. This port replaces
 * select(), so libnix's never runs; the hook still follows libnix's contract
 * (sources/socket/socket/__initsock.c, _sock_select) so the descriptors stay
 * correct if any other libnix routine uses it. */
static int __stdargs net_select(StdFileDes *sfd, int cmd, int mode,
		fd_set *set, unsigned long *nfds)
{
	if (cmd == SELCMD_PREPARE) {
		FD_SET(sfd->lx_sock, set);
		if ((unsigned long)sfd->lx_sock > *nfds)
			*nfds = sfd->lx_sock;
		return 0;
	}
	if (cmd == SELCMD_CHECK)
		return FD_ISSET(sfd->lx_sock, set) ? 1 : 0;
	if (cmd == SELCMD_POLL) {
		fd_set s;
		struct timeval tv;
		long n;

		FD_ZERO(&s);
		FD_SET(sfd->lx_sock, &s);
		tv.tv_sec = 0;
		tv.tv_usec = 0;
		n = WaitSelect(sfd->lx_sock + 1,
			mode == SELMODE_IN ? &s : NULL,
			mode == SELMODE_OUT ? &s : NULL,
			mode == SELMODE_EXC ? &s : NULL,
			&tv, NULL);
		return n == 1 ? 1 : 0;
	}
	return 0;
}

static struct _StdFileFx net_fx = {
	net_read, net_write, net_close, net_dup, net_fstat, net_select
};

/* Gives socket number sock a descriptor that uses net_fx. Returns the
 * descriptor, or -1 with errno set (the socket is closed then). */
static int wrap_socket(long sock, int family, int type, int protocol)
{
	StdFileDes *sfd = __allocfd();

	if (sfd == NULL) {
		CloseSocket(sock);
		errno = EMFILE;
		return -1;
	}
	if (sfd->lx_pos >= FD_SETSIZE) {
		sfd->lx_inuse = 0;
		CloseSocket(sock);
		errno = EMFILE;
		return -1;
	}
	sfd->lx_flags = LX_SOCKET;
	sfd->lx_inuse = 1;
	sfd->lx_sock = sock;
	sfd->lx_family = family;
	sfd->lx_domain = type;
	sfd->lx_protocol = protocol;
	sfd->lx_fx = &net_fx;
	nb_set(sfd->lx_pos, sock, 0);
	return sfd->lx_pos;
}

/* ---- POSIX functions --------------------------------------------------- */

int (socket)(int domain, int type, int protocol)
{
	long sock = socket(domain, type, protocol);

	if (sock < 0)
		return -1;
	return wrap_socket(sock, domain, type, protocol);
}

int (accept)(int s, struct sockaddr *addr, socklen_t *addrlen)
{
	StdFileDes *sfd = _lx_fhfromfd(s);
	long sock;

	if (sfd == NULL || !(sfd->lx_flags & LX_SOCKET)) {
		errno = EBADF;
		return -1;
	}
	sock = accept(sfd->lx_sock, addr, addrlen);
	if (sock < 0)
		return -1;
	return wrap_socket(sock, sfd->lx_family, sfd->lx_domain, sfd->lx_protocol);
}

int amiga_waitselect(int nfds, fd_set *readfds, fd_set *writefds,
		fd_set *exceptfds, struct timeval *timeout, ULONG *sigmask)
{
	fd_set rd, wr, ex;
	struct timeval tv, *tvp = NULL;
	long maxsock = -1, sock, n;
	int fd, count = 0;

	if (nfds < 0 || nfds > FD_SETSIZE) {
		errno = EINVAL;
		return -1;
	}
	FD_ZERO(&rd);
	FD_ZERO(&wr);
	FD_ZERO(&ex);

	/* Translate descriptors to the socket numbers WaitSelect() needs. */
	for (fd = 0; fd < nfds; fd++) {
		int r = readfds && FD_ISSET(fd, readfds);
		int w = writefds && FD_ISSET(fd, writefds);
		int x = exceptfds && FD_ISSET(fd, exceptfds);

		if (!r && !w && !x)
			continue;
		sock = sock_of(fd);
		if (sock < 0)
			return -1;
		if (sock >= FD_SETSIZE) {
			errno = EINVAL;
			return -1;
		}
		if (r)
			FD_SET(sock, &rd);
		if (w)
			FD_SET(sock, &wr);
		if (x)
			FD_SET(sock, &ex);
		if (sock > maxsock)
			maxsock = sock;
	}

	if (timeout != NULL) {
		tv.tv_sec = timeout->tv_sec;
		tv.tv_usec = timeout->tv_usec;
		tvp = &tv;
	}

	n = WaitSelect(maxsock + 1, &rd, &wr, &ex, tvp, sigmask);
	if (n < 0)
		return -1;

	/* Translate the result back. Only bits below nfds are touched, so a
	 * caller whose fd_set is smaller than ours is never written past. */
	for (fd = 0; fd < nfds; fd++) {
		int r = readfds && FD_ISSET(fd, readfds);
		int w = writefds && FD_ISSET(fd, writefds);
		int x = exceptfds && FD_ISSET(fd, exceptfds);

		if (!r && !w && !x)
			continue;
		sock = sock_of(fd);
		if (r) {
			if (FD_ISSET(sock, &rd))
				count++;
			else
				FD_CLR(fd, readfds);
		}
		if (w) {
			if (FD_ISSET(sock, &wr))
				count++;
			else
				FD_CLR(fd, writefds);
		}
		if (x) {
			if (FD_ISSET(sock, &ex))
				count++;
			else
				FD_CLR(fd, exceptfds);
		}
	}
	return count;
}

int (select)(int nfds, fd_set *readfds, fd_set *writefds, fd_set *exceptfds,
		struct timeval *timeout)
{
	return amiga_waitselect(nfds, readfds, writefds, exceptfds, timeout, NULL);
}

/* F_GETFL and F_SETFL, the only commands the server uses (dbutil.c
 * setnonblocking). On a socket, O_NONBLOCK maps to FIONBIO. On a file,
 * F_GETFL reports the open flags and setting O_NONBLOCK fails with ENODEV,
 * which setnonblocking() accepts for descriptors that cannot be
 * non-blocking. */
int fcntl(int fd, int cmd, ...)
{
	StdFileDes *sfd = _lx_fhfromfd(fd);
	va_list ap;
	int arg;

	if (sfd == NULL) {
		errno = EBADF;
		return -1;
	}
	va_start(ap, cmd);
	arg = va_arg(ap, int);
	va_end(ap);

	if (cmd == F_GETFL) {
		if (sfd->lx_flags & LX_SOCKET)
			return O_RDWR | (nb_get(fd, sfd->lx_sock) ? O_NONBLOCK : 0);
		return sfd->lx_oflags;
	}
	if (cmd == F_SETFL) {
		if (sfd->lx_flags & LX_SOCKET) {
			LONG on = (arg & O_NONBLOCK) ? 1 : 0;

			if (fd >= FD_SETSIZE) {
				errno = EBADF;
				return -1;
			}
			if (IoctlSocket(sfd->lx_sock, FIONBIO, (APTR)&on) < 0)
				return -1;
			nb_set(fd, sfd->lx_sock, on);
			return 0;
		}
		if (arg & O_NONBLOCK) {
			errno = ENODEV;
			return -1;
		}
		return 0;
	}
	errno = EINVAL;
	return -1;
}

/* socketpair() for AF_UNIX/AF_LOCAL stream sockets, built from two
 * connected 127.0.0.1 TCP sockets. Only the pair's own client is accepted:
 * the accepted connection's peer port must be the client's local port. */
int socketpair(int domain, int type, int protocol, int sv[2])
{
	struct sockaddr_in addr, local, peer;
	socklen_t len;
	long lsock, csock = -1, asock = -1;
	LONG one = 1;
	int fd0, fd1;

	(void)protocol;
	if ((domain != AF_UNIX && domain != AF_INET) || type != SOCK_STREAM) {
		errno = EOPNOTSUPP;
		return -1;
	}

	lsock = socket(AF_INET, SOCK_STREAM, 0);
	if (lsock < 0)
		return -1;
	memset(&addr, 0, sizeof(addr));
	addr.sin_family = AF_INET;
	addr.sin_addr.s_addr = htonl(0x7f000001UL); /* 127.0.0.1 */
	addr.sin_port = 0;
	len = sizeof(addr);
	if (bind(lsock, (struct sockaddr *)&addr, sizeof(addr)) < 0
	    || listen(lsock, 1) < 0
	    || getsockname(lsock, (struct sockaddr *)&addr, &len) < 0)
		goto fail;

	csock = socket(AF_INET, SOCK_STREAM, 0);
	if (csock < 0
	    || connect(csock, (struct sockaddr *)&addr, sizeof(addr)) < 0)
		goto fail;
	len = sizeof(local);
	if (getsockname(csock, (struct sockaddr *)&local, &len) < 0)
		goto fail;

	len = sizeof(peer);
	asock = accept(lsock, (struct sockaddr *)&peer, &len);
	if (asock < 0)
		goto fail;
	if (peer.sin_port != local.sin_port
	    || peer.sin_addr.s_addr != htonl(0x7f000001UL)) {
		errno = ECONNREFUSED;
		goto fail;
	}
	CloseSocket(lsock);
	lsock = -1;

	/* Interactive traffic: send small writes at once. */
	setsockopt(csock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
	setsockopt(asock, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));

	fd0 = wrap_socket(csock, AF_INET, SOCK_STREAM, 0);
	csock = -1;
	if (fd0 < 0)
		goto fail;
	fd1 = wrap_socket(asock, AF_INET, SOCK_STREAM, 0);
	asock = -1;
	if (fd1 < 0) {
		close(fd0);
		goto fail;
	}
	sv[0] = fd0;
	sv[1] = fd1;
	return 0;

fail:
	{
		int saved = errno;

		if (asock >= 0)
			CloseSocket(asock);
		if (csock >= 0)
			CloseSocket(csock);
		if (lsock >= 0)
			CloseSocket(lsock);
		errno = saved;
	}
	return -1;
}

long amiga_socket_to_id(int fd)
{
	long sock = sock_of(fd);

	if (sock < 0)
		return -1;
	return ReleaseCopyOfSocket(sock, UNIQUE_ID);
}

int amiga_socket_from_id(long id)
{
	long sock = ObtainSocket(id, AF_INET, SOCK_STREAM, 0);

	if (sock < 0)
		return -1;
	return wrap_socket(sock, AF_INET, SOCK_STREAM, 0);
}
