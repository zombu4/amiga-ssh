/*
 * amiga_main.c - entry point and listener of the AmigaOS port.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * sshd runs in one of two modes (docs/DESIGN.md, Process model):
 *
 *  - Listener (no -i). Waits for bsdsocket.library, listens on TCP port 22
 *    (or every -p given) and starts a new copy of itself for each
 *    connection: "<own path> -i -S <socket id>,<pipe id> <options>". The
 *    connection is handed over with ReleaseCopyOfSocket() and taken by the
 *    new process with ObtainSocket(). AmigaOS loads a separate copy of the
 *    program for that process, so it has its own global data, which
 *    Dropbear's one-session-per-process design needs.
 *
 *  - Session (-i). Takes the connection and runs Dropbear's inetd mode
 *    (main_inetd() in svr-main.c) on it.
 *
 * How the work is divided: main() below only prepares the process (no DOS
 * requesters, enough stack) and takes over the sockets named by -S. It then
 * calls Dropbear's own main(), renamed amiga_dropbear_main() by the
 * patches, which parses every option. In inetd mode Dropbear serves the
 * connection; otherwise it calls amiga_listener(), which takes the place of
 * Dropbear's fork()-based listener (main_noinetd(), left out with
 * NON_INETD_MODE 0 in localoptions.h).
 *
 * Unauthenticated connections are limited as upstream's listener does
 * (svr-main.c, main_noinetd): at most MAX_UNAUTH_CLIENTS in total and
 * MAX_UNAUTH_PER_IP per client address. Each session gets one end of a
 * loopback socket pair; Dropbear closes it after a successful login
 * (svr-auth.c, send_msg_userauth_success), and the process closes it when
 * it ends. Either way the listener's end becomes readable and the slot is
 * freed. A slot whose session never took its socket is freed after the
 * longest time a session may stay unauthenticated (AUTH_TIMEOUT).
 */
#include "includes.h"
#include "dbutil.h"
#include "dbrandom.h"
#include "runopts.h"

/* Amiga headers after Dropbear's: the inline headers define function-like
 * macros (Open, Close, Read, ...) that must not touch Dropbear's
 * declarations. proto/bsdsocket.h is not included: every socket call here
 * goes through libnix's descriptors (port/amiga_net.c). */
#include <exec/types.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <proto/exec.h>
#include <proto/dos.h>

#include "amiga_net.h"
#include "amiga_random.h"
#include "amiga_main.h"

/* The stack: port/amiga_stack.c, linked into sshd, gives every process of
 * the program (listener and sessions) 128 KB without a Stack command. */

/* ---- libnix socket start-up --------------------------------------------- */

/* libnix opens bsdsocket.library once, before main(), in __initsocket()
 * (sources/socket/socket/__initsock.c). If that fails it tries
 * socket.library (AS225), sets network_installed if either opened, and
 * leaves everything closed and SocketBase NULL otherwise, so that it can
 * be called again. Its exit routine closes whatever it opened. */
extern struct Library *SocketBase;
extern int network_installed;
extern void __initsocket(void);

/* ---- shared state --------------------------------------------------------- */

int amiga_session_fd = 0;
int amiga_session_childpipe = -1;

/* pr_WindowPtr as it was when the program started. A command runs in the
 * Shell's own process, so the old value is put back at exit. */
static APTR saved_windowptr;
static struct Process *self_process;

/* How long the listener waits before trying again, in ticks (1/50 s). */
#define NETWORK_RETRY_TICKS (5 * TICKS_PER_SECOND)
#define BIND_RETRY_TICKS (5 * TICKS_PER_SECOND)

/* Bytes of the seed file, the size dbrandom.c writes (INIT_SEED_SIZE,
 * dbrandom.c:40). */
#define SEED_FILE_BYTES 32

/* Seconds after which an unauthenticated slot is freed even without word
 * from its session: a session ends itself AUTH_TIMEOUT seconds after it
 * started if no login succeeded (common-session.c, checktimeouts). */
#define SLOT_EXPIRE_SECS (AUTH_TIMEOUT + 60)

int amiga_break_pending(void)
{
	/* SetSignal(0, mask) clears the bits in mask and returns the signals
	 * that were set before. */
	return (SetSignal(0, SIGBREAKF_CTRL_C) & SIGBREAKF_CTRL_C) ? 1 : 0;
}

static void restore_windowptr(void)
{
	self_process->pr_WindowPtr = saved_windowptr;
}

/* Waits ticks/50 seconds in one-second steps. Returns 1 at once if Ctrl-C
 * arrives, 0 after the full time. */
static int pause_ticks(LONG ticks)
{
	while (ticks > 0) {
		LONG step = ticks < TICKS_PER_SECOND ? ticks : TICKS_PER_SECOND;

		Delay(step);
		ticks -= step;
		if (amiga_break_pending()) {
			return 1;
		}
	}
	return 0;
}

/* ---- listener ------------------------------------------------------------- */

/* Set by the SIGINT handler: libnix raises SIGINT from read() and write()
 * when Ctrl-C is set (sources/nix/signal/__chkabort.c). */
static volatile int listener_stop;

static void listener_sigint(int UNUSED(sig))
{
	listener_stop = 1;
}

/* One listening address. */
struct listen_sock {
	int fd;
	struct sockaddr_in addr;
};

/* One unauthenticated session. pipe is -1 for a free slot. */
struct unauth_slot {
	int pipe;
	struct in_addr peer;
	time_t started;
};

/* Waits until bsdsocket.library can be opened, so the listener can be
 * started from S:User-Startup before the TCP/IP stack is up. */
static void wait_for_network(void)
{
	int told = 0;

	while (SocketBase == NULL) {
		if (network_installed) {
			/* libnix opened socket.library (AS225) instead; amiga_net.c
			 * needs bsdsocket.library */
			dropbear_exit("bsdsocket.library is not available");
		}
		__initsocket();
		if (SocketBase != NULL) {
			break;
		}
		if (!told) {
			dropbear_log(LOG_INFO, "Waiting for bsdsocket.library");
			told = 1;
		}
		if (pause_ticks(NETWORK_RETRY_TICKS)) {
			dropbear_close("Terminated by Ctrl-C");
		}
	}
}

/* Characters that are refused in the program path and in the options the
 * listener passes on to each session. The command line is read by the
 * Shell, which expands $variables and `commands` (Shell 45 and later also
 * inside arguments) and treats '"' and '*' specially, and then split by
 * libnix (sources/nix20/misc/__nocommandline.c), which treats '"' and '*'
 * specially; a line break would end the command. With none of these
 * present, wrapping each word in double quotes passes it on unchanged. */
static const char *bad_command_chars = "\"*$`\n\r";

/* Checks one word for the command line; exits with a message if it holds a
 * refused character. */
static void check_command_word(const char *word, const char *what)
{
	if (strpbrk(word, bad_command_chars) != NULL) {
		dropbear_exit("The %s '%s' may not contain any of \" * $ ` "
				"or a line break", what, word);
	}
}

/* Returns the full path of this program, so each session runs the same
 * file. argv0 is the name the Shell started it with: libnix reads it with
 * GetProgramName() in full (sources/nix20/misc/__nocommandline.c). The
 * directory comes from GetProgramDir(), a lock on the directory the
 * program was loaded from, or 0 for a resident command, which the Shell
 * then finds by its name. */
static char *own_path(const char *argv0)
{
	char path[512];
	BPTR dir = GetProgramDir();

	if (dir == 0) {
		return m_strdup(argv0);
	}
	if (!NameFromLock(dir, (STRPTR)path, sizeof(path))
			|| !AddPart((STRPTR)path, FilePart((CONST_STRPTR)argv0),
				sizeof(path))) {
		dropbear_exit("Can't find the path of %s", argv0);
	}
	return m_strdup(path);
}

/* Builds the session's command line:
 *   "<path>" -i -S <sockid>,<pipeid> "<option>" ...
 * argv[1..argc-1] are the listener's own options, passed on unchanged, so
 * the session uses the same keys, timeouts and so on. Every word was
 * checked with check_command_word(). Returns an m_malloc()ed string. */
static char *session_command(const char *path, long sockid, long pipeid,
		int argc, char **argv)
{
	size_t len = strlen(path) + 64;
	char *cmd, *p;
	int i;

	for (i = 1; i < argc; i++) {
		len += strlen(argv[i]) + 3;
	}
	cmd = m_malloc(len);
	p = cmd;
	p += snprintf(p, len, "\"%s\" -i -S %ld,%ld", path, sockid, pipeid);
	for (i = 1; i < argc; i++) {
		p += snprintf(p, len - (p - cmd), " \"%s\"", argv[i]);
	}
	return cmd;
}

/* Parses svr_opts.ports[i] and svr_opts.addresses[i] (svr-runopts.c,
 * addportandaddress) into a socket address. The listener takes a decimal
 * port and either no address (all addresses) or a numeric IPv4 address. */
static void listen_address(unsigned int i, struct sockaddr_in *sin)
{
	const char *portstr = svr_opts.ports[i];
	const char *addrstr = svr_opts.addresses[i];
	char *end = NULL;
	unsigned long port;

	port = strtoul(portstr, &end, 10);
	if (portstr[0] == '\0' || *end != '\0' || port == 0 || port > 65535) {
		dropbear_exit("Bad port '%s'", portstr);
	}
	memset(sin, 0, sizeof(*sin));
	sin->sin_family = AF_INET;
	sin->sin_port = htons((unsigned short)port);
	if (addrstr == NULL || addrstr[0] == '\0') {
		sin->sin_addr.s_addr = htonl(INADDR_ANY);
	} else if (inet_aton(addrstr, &sin->sin_addr) == 0) {
		dropbear_exit("Listen address must be a numeric IPv4 address: '%s'",
				addrstr);
	}
}

/* Opens a listening socket on sin. While that fails (for example while an
 * old listener still holds the port) it tries again every few seconds;
 * Ctrl-C ends the wait. */
static int open_listener(const struct sockaddr_in *sin)
{
	int told = 0;

	for (;;) {
		int fd = socket(AF_INET, SOCK_STREAM, 0);
		int err;

		if (fd >= 0) {
			int on = 1;

			setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &on, sizeof(on));
			if (bind(fd, (const struct sockaddr *)sin, sizeof(*sin)) == 0
					&& listen(fd, DROPBEAR_LISTEN_BACKLOG) == 0) {
				dropbear_log(LOG_INFO, "Listening on %s port %u",
						inet_ntoa(sin->sin_addr), ntohs(sin->sin_port));
				return fd;
			}
			err = errno;
			m_close(fd);
		} else {
			err = errno;
		}
		if (!told) {
			dropbear_log(LOG_WARNING, "Can't listen on %s port %u: %s; "
					"trying again", inet_ntoa(sin->sin_addr),
					ntohs(sin->sin_port), strerror(err));
			told = 1;
		}
		if (pause_ticks(BIND_RETRY_TICKS)) {
			dropbear_close("Terminated by Ctrl-C");
		}
	}
}

/* Adds the connection's arrival time and peer address to the random pool
 * and writes fresh output back to the seed file, which the new session
 * reads when it starts (dbrandom.c, seedrandom). The file is written the
 * way dbrandom.c's write_urandom() writes it; a failure (for example the
 * file being locked by a session that is rewriting it) is not an error. */
static void stir_random(const struct sockaddr_in *peer)
{
	struct {
		ULONG eclock;
		struct sockaddr_in peer;
	} sample;
	unsigned char seed[SEED_FILE_BYTES];
	FILE *f;

	sample.eclock = amiga_eclock_now();
	sample.peer = *peer;
	addrandom((const unsigned char *)&sample, sizeof(sample));

	genrandom(seed, sizeof(seed));
	f = fopen(DROPBEAR_URANDOM_DEV, "w");
	if (f != NULL) {
		fwrite(seed, sizeof(seed), 1, f);
		fclose(f);
	}
	m_burn(seed, sizeof(seed));
	m_burn(&sample, sizeof(sample));
}

/* Takes back a socket that was handed over with amiga_socket_to_id() but
 * not picked up, and closes it. */
static void reclaim_socket(long id)
{
	int fd = amiga_socket_from_id(id);

	if (fd >= 0) {
		m_close(fd);
	}
}

/* Starts a session process for the connection in fd, with pipefd as its
 * end of the pre-authentication pair. Both descriptors stay open in the
 * listener; the caller closes them. Returns DROPBEAR_SUCCESS or
 * DROPBEAR_FAILURE. */
static int start_session(const char *path, int fd, int pipefd,
		int argc, char **argv)
{
	long sockid, pipeid;
	BPTR in, out;
	char *cmd;
	LONG rc;

	sockid = amiga_socket_to_id(fd);
	if (sockid < 0) {
		dropbear_log(LOG_WARNING, "Can't hand over the connection: %s",
				strerror(errno));
		return DROPBEAR_FAILURE;
	}
	pipeid = amiga_socket_to_id(pipefd);
	if (pipeid < 0) {
		dropbear_log(LOG_WARNING, "Can't hand over the connection: %s",
				strerror(errno));
		reclaim_socket(sockid);
		return DROPBEAR_FAILURE;
	}

	/* The session has no console: input and output are NIL:. System()
	 * needs two different handles (dos.doc, SystemTagList). */
	in = Open((CONST_STRPTR)"NIL:", MODE_OLDFILE);
	out = Open((CONST_STRPTR)"NIL:", MODE_NEWFILE);
	if (in == 0 || out == 0) {
		dropbear_log(LOG_WARNING, "Can't open NIL:");
		if (in != 0) {
			Close(in);
		}
		if (out != 0) {
			Close(out);
		}
		reclaim_socket(sockid);
		reclaim_socket(pipeid);
		return DROPBEAR_FAILURE;
	}

	cmd = session_command(path, sockid, pipeid, argc, argv);

	/* SYS_Asynch: start the Shell process and return at once; it closes
	 * both handles when the command ends. The NP_ tags pass through to
	 * CreateNewProc() (dos.doc, SystemTagList): no console task, so
	 * nothing the session writes to "*" reaches the listener's window, and
	 * no DOS requesters while the program is loaded. */
	rc = SystemTags((CONST_STRPTR)cmd,
			SYS_Input, in,
			SYS_Output, out,
			SYS_Asynch, TRUE,
			NP_ConsoleTask, NULL,
			NP_WindowPtr, (APTR)-1,
			NP_Name, (ULONG)"sshd session",
			TAG_END);
	m_free(cmd);

	if (rc == -1) {
		/* On -1 the handles are still the caller's, except that dos.library
		 * V45 and older could release SYS_Input in some error cases
		 * (dos.doc, SystemTagList BUGS); there the input handle is left
		 * open rather than risk closing it twice. */
		dropbear_log(LOG_WARNING, "Can't start a session process");
		Close(out);
		if (DOSBase->dl_lib.lib_Version >= 47) {
			Close(in);
		}
		reclaim_socket(sockid);
		reclaim_socket(pipeid);
		return DROPBEAR_FAILURE;
	}
	return DROPBEAR_SUCCESS;
}

/* Accepts one connection on lfd and starts its session, unless the limits
 * on unauthenticated connections are reached. */
static void accept_connection(int lfd, struct unauth_slot *slots,
		const char *path, int argc, char **argv)
{
	struct sockaddr_in peer;
	socklen_t peerlen = sizeof(peer);
	unsigned int i, total = 0, same_peer = 0;
	int free_slot = -1;
	int fd, pair[2];

	fd = accept(lfd, (struct sockaddr *)&peer, &peerlen);
	if (fd < 0) {
		return;
	}

	for (i = 0; i < MAX_UNAUTH_CLIENTS; i++) {
		if (slots[i].pipe >= 0) {
			total++;
			if (slots[i].peer.s_addr == peer.sin_addr.s_addr) {
				same_peer++;
			}
		} else if (free_slot < 0) {
			free_slot = (int)i;
		}
	}
	if (free_slot < 0 || total >= MAX_UNAUTH_CLIENTS
			|| same_peer >= MAX_UNAUTH_PER_IP) {
		/* as upstream: close the connection without a word */
		m_close(fd);
		return;
	}

	stir_random(&peer);

	if (socketpair(AF_UNIX, SOCK_STREAM, 0, pair) < 0) {
		dropbear_log(LOG_WARNING, "Can't make a loopback socket pair: %s",
				strerror(errno));
		m_close(fd);
		return;
	}

	if (start_session(path, fd, pair[1], argc, argv) == DROPBEAR_SUCCESS) {
		slots[free_slot].pipe = pair[0];
		slots[free_slot].peer = peer.sin_addr;
		slots[free_slot].started = monotonic_now();
	} else {
		m_close(pair[0]);
	}
	/* The session has its own copies now. */
	m_close(pair[1]);
	m_close(fd);
}

void amiga_listener(int argc, char **argv)
{
	struct listen_sock socks[DROPBEAR_MAX_PORTS];
	struct unauth_slot slots[MAX_UNAUTH_CLIENTS];
	unsigned int nsocks, i;
	char *path;
	int a;

	/* Ctrl-C while writing a log line or similar raises SIGINT in libnix;
	 * without a handler libnix would exit at once. */
	if (signal(SIGINT, listener_sigint) == SIG_ERR) {
		dropbear_exit("signal() error");
	}

	/* Everything that goes onto the sessions' command line is checked
	 * before anything is opened. */
	path = own_path(argv[0]);
	check_command_word(path, "program path");
	for (a = 1; a < argc; a++) {
		check_command_word(argv[a], "option");
	}

	wait_for_network();

	/* Start the random pool from the seed file and the system samples;
	 * each connection adds to it (stir_random). */
	seedrandom();

	nsocks = svr_opts.portcount;
	if (nsocks > DROPBEAR_MAX_PORTS) {
		nsocks = DROPBEAR_MAX_PORTS;
	}
	for (i = 0; i < nsocks; i++) {
		listen_address(i, &socks[i].addr);
	}
	for (i = 0; i < nsocks; i++) {
		socks[i].fd = open_listener(&socks[i].addr);
	}

	for (i = 0; i < MAX_UNAUTH_CLIENTS; i++) {
		slots[i].pipe = -1;
	}

	for (;;) {
		fd_set readfds;
		struct timeval tv, *tvp = NULL;
		int maxfd = -1, n;
		time_t now;

		FD_ZERO(&readfds);
		for (i = 0; i < nsocks; i++) {
			FD_SET(socks[i].fd, &readfds);
			maxfd = MAX(maxfd, socks[i].fd);
		}
		for (i = 0; i < MAX_UNAUTH_CLIENTS; i++) {
			if (slots[i].pipe >= 0) {
				FD_SET(slots[i].pipe, &readfds);
				maxfd = MAX(maxfd, slots[i].pipe);
				/* wake up now and then to expire stale slots */
				tv.tv_sec = 60;
				tv.tv_usec = 0;
				tvp = &tv;
			}
		}

		n = amiga_waitselect(maxfd + 1, &readfds, NULL, NULL, tvp, NULL);

		/* WaitSelect() fails with EINTR on Ctrl-C and leaves the break
		 * signal set (bsdsocket.doc, WaitSelect). */
		if (listener_stop || (n < 0 && errno == EINTR
					&& amiga_break_pending())) {
			dropbear_close("Terminated by Ctrl-C");
		}
		if (n < 0) {
			if (errno == EINTR) {
				continue;
			}
			dropbear_exit("Error waiting for connections: %s",
					strerror(errno));
		}

		/* A readable pre-authentication socket means the session has
		 * logged in or ended. */
		now = monotonic_now();
		for (i = 0; i < MAX_UNAUTH_CLIENTS; i++) {
			if (slots[i].pipe < 0) {
				continue;
			}
			if ((n > 0 && FD_ISSET(slots[i].pipe, &readfds))
					|| now - slots[i].started > SLOT_EXPIRE_SECS) {
				m_close(slots[i].pipe);
				slots[i].pipe = -1;
			}
		}

		if (n <= 0) {
			continue;
		}
		for (i = 0; i < nsocks; i++) {
			if (FD_ISSET(socks[i].fd, &readfds)) {
				accept_connection(socks[i].fd, slots, path, argc, argv);
			}
		}
	}
}

/* ---- entry point ---------------------------------------------------------- */

/* Parses "<sockid>,<pipeid>" from -S. Returns 0 on success. */
static int parse_ids(const char *arg, long *sockid, long *pipeid)
{
	char *end = NULL;

	*sockid = strtol(arg, &end, 10);
	if (end == arg || *end != ',' || *sockid < 0) {
		return -1;
	}
	arg = end + 1;
	*pipeid = strtol(arg, &end, 10);
	if (end == arg || *end != '\0' || *pipeid < 0) {
		return -1;
	}
	return 0;
}

/* Returns 1 if descriptor fd is an open socket. */
static int is_socket(int fd)
{
	StdFileDes *sfd = _lx_fhfromfd(fd);

	return sfd != NULL && (sfd->lx_flags & LX_SOCKET) != 0;
}

int main(int argc, char **argv)
{
	/* No DOS requester may ever wait for a click on a machine that is
	 * used over the network: with pr_WindowPtr -1, DOS returns an error
	 * instead (dos.doc, NameFromLock and others). */
	self_process = (struct Process *)FindTask(NULL);
	saved_windowptr = self_process->pr_WindowPtr;
	self_process->pr_WindowPtr = (APTR)-1;
	atexit(restore_windowptr);

	if (argc >= 2 && strcmp(argv[1], "-i") == 0) {
		if (SocketBase == NULL) {
			fprintf(stderr, "sshd: bsdsocket.library is not available\n");
			return RETURN_FAIL;
		}
		if (argc >= 4 && strcmp(argv[2], "-S") == 0) {
			/* Started by the listener: take the connection and the
			 * pre-authentication socket, then drop "-S <ids>" so that
			 * Dropbear sees only its own options. */
			long sockid, pipeid;
			int i;

			if (parse_ids(argv[3], &sockid, &pipeid) != 0) {
				fprintf(stderr, "sshd: bad -S argument '%s'\n", argv[3]);
				return RETURN_FAIL;
			}
			amiga_session_fd = amiga_socket_from_id(sockid);
			if (amiga_session_fd < 0) {
				fprintf(stderr, "sshd: can't take over the connection: %s\n",
						strerror(errno));
				return RETURN_FAIL;
			}
			/* Without it the session still works; the listener then
			 * frees the slot after SLOT_EXPIRE_SECS. */
			amiga_session_childpipe = amiga_socket_from_id(pipeid);
			for (i = 2; i + 2 <= argc; i++) {
				argv[i] = argv[i + 2];
			}
			argc -= 2;
		} else if (!is_socket(0)) {
			/* -i without -S: a superserver must have put the connection
			 * on descriptor 0 (libnix does this, __initsock.c,
			 * init_inet_daemon). */
			fprintf(stderr, "sshd: -i needs a connection on descriptor 0 "
					"or -S\n");
			return RETURN_FAIL;
		}
	}

	return amiga_dropbear_main(argc, argv);
}
