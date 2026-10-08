/*
 * amiga_console.c - console handler for one SSH channel (program
 * "sshd-console").
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * The server (port/amiga_session.c) starts one copy of this program for
 * every "shell" or "exec" channel. It is a separate program, loaded with
 * LoadSeg() and started with NP_FreeSeglist, so its code stays in memory
 * for as long as it runs, even if the server process ends first.
 *
 * Command line (NP_Arguments): "<socket id> <control block address in hex>".
 *  - The socket id names the far end of the channel's socket pair, handed
 *    over by the server with ReleaseCopyOfSocket(); it is taken here with
 *    ObtainSocket().
 *  - The control block (struct amiga_child) is shared with the server. It
 *    carries the command, the pty flag, the home directory, break requests
 *    from the server, and the return code back to it. Every access to a
 *    shared field happens between Forbid() and Permit().
 *
 * What it does:
 *  - It is a minimal AmigaDOS console handler on a private message port.
 *    The shell or command runs with two FileHandles whose fh_Type is that
 *    port, and with the port as its console task, so "*" opens there too.
 *    The DOS packets it sends (READ, WRITE, WAIT_CHAR, SCREEN_MODE, ...) are
 *    answered here, and the bytes move to and from the socket.
 *  - A helper process calls SystemTagList() and blocks there until the shell
 *    or command returns. The handler itself never calls SystemTagList(): it
 *    has to keep answering the packets that call produces. The port is
 *    private, not this process's pr_MsgPort, so the handler's own DOS calls
 *    and the shell's console packets can never be mixed up.
 *  - With a terminal (pty) it behaves like a line-mode console: local echo,
 *    line editing, Ctrl-C/D/E/F as break signals, LF sent as CR LF. Without
 *    one, bytes pass through unchanged.
 *
 * The program uses libnix's startup code, which waits for a Workbench
 * message whenever the process has no CLI structure (ncrt0.S, fromWB); the
 * server therefore starts it with NP_Cli TRUE. bsdsocket.library is opened
 * here and called directly; libnix's select() and fcntl() are never used
 * (see the header comment of port/amiga_net.c).
 */

/* POSIX headers first: proto/bsdsocket.h defines function-like macros that
 * would break their prototypes (see port/amiga_net.c). */
#include <sys/types.h>
#include <sys/time.h>
#include <sys/socket.h>
#include <sys/filio.h>
#include <netinet/in.h>
#include <errno.h>
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <exec/lists.h>
#include <exec/ports.h>
#include <exec/io.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <dos/dostags.h>
#include <dos/var.h>
#include <dos/dosasl.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/dos.h>
#include <libraries/bsdsocket.h>
#include <proto/bsdsocket.h>

/* ---- the control block shared with the server ------------------------- */

/* One definition for both programs (port/amiga_child.h). The server stores
 * AMIGA_CHILD_MAGIC and sizeof(struct amiga_child) in the first two fields,
 * and this program refuses a block whose values differ. */
#include "amiga_child.h"
#include "amiga_paths.h"

/* ---- sizes and limits --------------------------------------------------- */

/* The three ring buffer sizes must be powers of two: indexes wrap with a
 * mask, which avoids a division (a utility.library call on the 68000) for
 * every byte. The typedefs below fail to compile otherwise. */
#define IN_SIZE		512	/* bytes received, not yet processed */
#define READY_SIZE	1024	/* bytes ready for the reader */
#define OUT_SIZE	16384	/* bytes waiting to be sent */
typedef char in_size_is_pow2[(IN_SIZE & (IN_SIZE - 1)) == 0 ? 1 : -1];
typedef char ready_size_is_pow2[(READY_SIZE & (READY_SIZE - 1)) == 0 ? 1 : -1];
typedef char out_size_is_pow2[(OUT_SIZE & (OUT_SIZE - 1)) == 0 ? 1 : -1];
#define LINE_MAX	256	/* cooked-mode line, including the LF */
/* Room the output buffer must have before one cooked-mode input byte is
 * processed: Ctrl-U echoes "\b \b" for every character of the line. */
#define ECHO_RESERVE	(3 * LINE_MAX)

#define SHELL_STACK	20000	/* stack of the shell/command process */
#define HELPER_STACK	8192	/* stack of the helper process */

#define SPAWN_SECS	10	/* shell: wait this long for NewShell's "*" */
#define DRAIN_SECS	5	/* after the session: wait for open handles */
#define FLUSH_SECS	10	/* at the end: give up sending after this
				 * long without progress */

/* Byte values. */
#define CH_BS		0x08
#define CH_LF		0x0a
#define CH_CR		0x0d
#define CH_CTRL_U	0x15
#define CH_ESC		0x1b
#define CH_CTRL_BSL	0x1c	/* Ctrl-\: end of file, as on CON: */
#define CH_DEL		0x7f
#define CH_CSI		0x9b	/* Amiga 8-bit control sequence introducer */

/* ---- state -------------------------------------------------------------- */

struct Library *SocketBase;		/* used by proto/bsdsocket.h */

static struct amiga_child *blk;		/* the shared control block */
static LONG sock = -1;			/* the channel socket */
static struct MsgPort *hport;		/* private handler port */
static struct MsgPort *tport;		/* replies of WAIT_CHAR timers */
static LONG ctl_sigbit = -1;		/* server -> handler requests */
static LONG helper_sigbit = -1;		/* helper -> handler: finished */
static struct Task *self;		/* this process */

static int pty;				/* copy of blk->pty */
static int raw;				/* SCREEN_MODE raw in effect */

/* A byte ring buffer. */
struct ring {
	UBYTE *buf;
	ULONG size;
	ULONG head;			/* index of the oldest byte */
	ULONG count;			/* bytes stored */
};

static UBYTE in_buf[IN_SIZE], ready_buf[READY_SIZE], out_buf[OUT_SIZE];
static struct ring in_ring = { in_buf, IN_SIZE, 0, 0 };
static struct ring ready_ring = { ready_buf, READY_SIZE, 0, 0 };
static struct ring out_ring = { out_buf, OUT_SIZE, 0, 0 };

static UBYTE line[LINE_MAX];		/* cooked-mode line being edited */
static ULONG line_len;
static int last_in_cr;			/* previous input byte was CR */
static int last_out_cr;			/* previous output byte was CR */
static int eof_mark;			/* Ctrl-\ typed on an empty line */

/* Control sequences in the program's output (terminal sessions only). A
 * sequence is collected until its final byte, so the ones that are
 * private to the Amiga console can be answered here instead of being sent
 * to the client's terminal (console.doc, private Amiga sequences):
 *   <CSI>0 q   window status request: answered with the window bounds
 *   <CSI>n{    set raw events, <CSI>n}  reset raw events: n = 12 asks
 *              for a report when the window is resized
 * A client terminal would read <CSI>0 q as "set cursor style". */
#define SEQ_MAX 24
static UBYTE seq[SEQ_MAX];		/* parameters and intermediates */
static int seq_len;
static int out_state;			/* 0 text, 1 after ESC, 2 in a sequence */
static int wsr_pending;			/* a window status request to answer */
static int resize_events;		/* the program asked for class 12 */
static int resize_pending;		/* a resize report to deliver */

/* Raw input event class for "window resized" (devices/inputevent.h,
 * IECLASS_SIZEWINDOW). */
#define EVENT_RESIZE 12
static int input_eof;			/* the client sent EOF (recv() == 0) */
static int gone;			/* the client cannot be reached */
static ULONG gone_at;

static struct List readq;		/* queued READ and WAIT_CHAR packets */
static struct List writeq;		/* WRITE packets not yet buffered */

static struct Task *reader_task;	/* gets Ctrl-C (last reader) */
static struct Task *writer_task;	/* fallback if nobody has read */

static BPTR h_in, h_out;		/* handles passed to SystemTagList() */
static LONG opens;			/* handles on the port not yet ENDed */
static int saw_find;			/* something opened "*" on the port */

static int helper_started;		/* the helper process exists */
static int helper_done;			/* it returned (h_done seen) */
static ULONG helper_done_at;
static LONG helper_rc;

/* Written by the helper process, read here; both under Forbid(). */
static LONG h_done, h_rc, h_closed;

static int finishing;			/* the session is over: flushing */
static ULONG flush_progress_at;
static int finished;			/* rc published, socket closed */
static ULONG finished_at;

/* ---- small helpers ------------------------------------------------------ */

/* Seconds since 1978 from DateStamp(); differences of these values stay
 * correct across the wrap of ULONG. */
static ULONG now_secs(void)
{
	struct DateStamp ds;

	DateStamp(&ds);
	return (ULONG)ds.ds_Days * 86400UL + (ULONG)ds.ds_Minute * 60UL
		+ (ULONG)ds.ds_Tick / TICKS_PER_SECOND;
}

static void list_init(struct List *l)
{
	l->lh_Head = (struct Node *)&l->lh_Tail;
	l->lh_Tail = NULL;
	l->lh_TailPred = (struct Node *)&l->lh_Head;
}

static int list_empty(const struct List *l)
{
	return l->lh_Head->ln_Succ == NULL;
}

static ULONG ring_free(const struct ring *r)
{
	return r->size - r->count;
}

static void ring_put(struct ring *r, UBYTE c)
{
	r->buf[(r->head + r->count) & (r->size - 1)] = c;
	r->count++;
}

static UBYTE ring_get(struct ring *r)
{
	UBYTE c = r->buf[r->head];

	r->head = (r->head + 1) & (r->size - 1);
	r->count--;
	return c;
}

static void ring_clear(struct ring *r)
{
	r->head = 0;
	r->count = 0;
}

/* ---- break signals ------------------------------------------------------ */

/* True if t is a task exec knows (ready or waiting). Called under Forbid(),
 * so the answer stays valid until the matching Permit(). */
static int task_listed(struct Task *t)
{
	struct Node *n;

	for (n = SysBase->TaskReady.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
		if ((struct Task *)n == t)
			return 1;
	for (n = SysBase->TaskWait.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ)
		if ((struct Task *)n == t)
			return 1;
	return 0;
}

/* Sends break signals to the task that last read from the console (or, if
 * nothing has read yet, the one that last wrote), as the Amiga console
 * does with Ctrl-C. A task that has ended is not signalled: the check and
 * the Signal() happen inside one Forbid(). */
static void send_break(ULONG bits)
{
	struct Task *t;

	bits &= SIGBREAKF_CTRL_C | SIGBREAKF_CTRL_D | SIGBREAKF_CTRL_E
		| SIGBREAKF_CTRL_F;
	if (bits == 0)
		return;
	Forbid();
	t = reader_task != NULL ? reader_task : writer_task;
	if (t != NULL && t != self && task_listed(t))
		Signal(t, bits);
	Permit();
}

/* The task behind a packet's reply port, or NULL if that port does not
 * signal a task (a soft-interrupt port, for example). */
static struct Task *packet_task(const struct DosPacket *pkt)
{
	const struct MsgPort *p = pkt->dp_Port;

	if (p != NULL && (p->mp_Flags & PF_ACTION) == PA_SIGNAL)
		return (struct Task *)p->mp_SigTask;
	return NULL;
}

/* ---- DOS packets -------------------------------------------------------- */

/* A packet arrives as an exec message whose ln_Name points to the
 * DosPacket (the StandardPacket convention). */
static struct DosPacket *packet_of(struct Message *msg)
{
	return (struct DosPacket *)msg->mn_Node.ln_Name;
}

/* Returns a packet to its sender, as ReplyPkt() does but with this
 * handler's private port in dp_Port. */
static void reply_packet(struct DosPacket *pkt, LONG res1, LONG res2)
{
	struct MsgPort *to = pkt->dp_Port;

	pkt->dp_Res1 = res1;
	pkt->dp_Res2 = res2;
	pkt->dp_Port = hport;
	PutMsg(to, pkt->dp_Link);
}

/* A WAIT_CHAR packet keeps its timer request in dp_Res2 while queued. This
 * cancels and frees it. */
static void stop_timer(struct DosPacket *pkt)
{
	struct timerequest *tr = (struct timerequest *)pkt->dp_Res2;

	if (tr != NULL) {
		AbortIO((struct IORequest *)tr);
		WaitIO((struct IORequest *)tr);
		CloseDevice((struct IORequest *)tr);
		DeleteIORequest((struct IORequest *)tr);
		pkt->dp_Res2 = 0;
	}
}

/* ---- output (to the socket) --------------------------------------------- */

/* Output can still reach the client. */
static int output_open(void)
{
	return sock >= 0 && !gone;
}

/* Queues echo bytes; the caller made sure there is room. Dropped once the
 * client is gone. */
static void echo(const char *s, ULONG n)
{
	ULONG i;

	if (!output_open())
		return;
	for (i = 0; i < n; i++)
		ring_put(&out_ring, (UBYTE)s[i]);
}

/* Puts one byte of ordinary terminal output: LF not preceded by CR becomes
 * CR LF. The caller made sure there are 2 bytes of room. */
static void put_text(UBYTE c)
{
	if (c == CH_LF && !last_out_cr) {
		ring_put(&out_ring, CH_CR);
		ring_put(&out_ring, CH_LF);
	} else {
		ring_put(&out_ring, c);
	}
	last_out_cr = (c == CH_CR);
}

/* 1 if the collected parameters (seq, seq_len) list the number n. */
static int seq_has_param(int n)
{
	int i = 0;

	while (i < seq_len) {
		int v = 0, digits = 0;

		while (i < seq_len && seq[i] >= '0' && seq[i] <= '9') {
			v = v * 10 + (seq[i] - '0');
			i++;
			digits++;
		}
		if (digits > 0 && v == n)
			return 1;
		while (i < seq_len && seq[i] != ';')
			i++;
		i++;			/* past ';' */
	}
	return 0;
}

/* Acts on a complete sequence with final byte c. Returns 1 if it was an
 * Amiga-private one that is answered here, 0 if it goes to the client. */
static int private_sequence(UBYTE c)
{
	if (c == 'q' && ((seq_len == 2 && seq[0] == '0' && seq[1] == ' ')
			 || (seq_len == 1 && seq[0] == ' '))) {
		wsr_pending = 1;
		return 1;
	}
	if (c == '{' || c == '}') {
		if (seq_has_param(EVENT_RESIZE))
			resize_events = (c == '{');
		return 1;	/* raw events have no meaning for the client */
	}
	return 0;
}

/* Moves up to n bytes of program output into the output buffer and returns
 * how many it took. With a terminal, LF not preceded by CR becomes CR LF,
 * the Amiga CSI byte becomes ESC '[' (console.doc: "CSI is either 9B or
 * Esc["), so the client's terminal sees the 7-bit form, and the
 * Amiga-private sequences above are answered here. A sequence split
 * between two writes is completed by the next one. */
static LONG take_output(const UBYTE *p, LONG n)
{
	LONG i;

	for (i = 0; i < n; i++) {
		UBYTE c = p[i];

		if (!pty) {
			if (ring_free(&out_ring) < 1)
				break;
			ring_put(&out_ring, c);
			continue;
		}
		if (out_state == 0) {
			if (c == CH_ESC) {
				out_state = 1;
			} else if (c == CH_CSI) {
				out_state = 2;
				seq_len = 0;
			} else {
				if (ring_free(&out_ring) < 2)
					break;
				put_text(c);
			}
		} else if (out_state == 1) {
			if (c == '[') {
				out_state = 2;
				seq_len = 0;
			} else {
				/* A lone ESC: send it, then this byte as text. */
				if (ring_free(&out_ring) < 3)
					break;
				ring_put(&out_ring, CH_ESC);
				out_state = 0;
				if (c == CH_ESC)
					out_state = 1;
				else if (c == CH_CSI) {
					out_state = 2;
					seq_len = 0;
				} else
					put_text(c);
			}
		} else if (c >= 0x20 && c <= 0x3f && seq_len < SEQ_MAX) {
			seq[seq_len++] = c;	/* parameter or intermediate */
		} else {
			/* Final byte (0x40-0x7e), or a sequence too long or broken
			 * to be one: pass it on unless it is answered here. */
			int final = (c >= 0x40 && c <= 0x7e);

			if (ring_free(&out_ring) < (ULONG)seq_len + 4)
				break;
			out_state = 0;
			if (final && private_sequence(c))
				continue;
			ring_put(&out_ring, CH_ESC);
			ring_put(&out_ring, '[');
			{
				int k;

				for (k = 0; k < seq_len; k++)
					ring_put(&out_ring, seq[k]);
			}
			if (final)
				ring_put(&out_ring, c);
			else
				put_text(c);
			last_out_cr = 0;
		}
	}
	return i;
}

/* Answers every queued WRITE as written, without sending its bytes. */
static void discard_writes(void)
{
	struct Message *msg;

	while ((msg = (struct Message *)RemHead(&writeq)) != NULL) {
		struct DosPacket *pkt = packet_of(msg);

		reply_packet(pkt, pkt->dp_Arg3, 0);
	}
}

/* Moves queued WRITE data into the output buffer, oldest packet first, and
 * answers each packet once all of its bytes are buffered. dp_Res1 counts
 * the bytes taken so far. */
static void serve_writes(void)
{
	struct Message *msg;

	if (!output_open()) {
		discard_writes();
		return;
	}
	while (!list_empty(&writeq)) {
		struct DosPacket *pkt;

		msg = (struct Message *)writeq.lh_Head;
		pkt = packet_of(msg);
		pkt->dp_Res1 += take_output((const UBYTE *)pkt->dp_Arg2
			+ pkt->dp_Res1, pkt->dp_Arg3 - pkt->dp_Res1);
		if (pkt->dp_Res1 < pkt->dp_Arg3)
			break;
		Remove(&msg->mn_Node);
		reply_packet(pkt, pkt->dp_Arg3, 0);
	}
}

static void go_gone(void);

/* Bytes handed to the socket so far; move_output() uses it to see whether
 * a pass made progress. */
static ULONG bytes_sent;

/* Sends as much buffered output as the socket takes without blocking.
 * EWOULDBLOCK and EINTR mean "later"; any other error means the client
 * cannot be reached any more. */
static void pump_send(void)
{
	while (out_ring.count > 0 && output_open()) {
		ULONG chunk = out_ring.size - out_ring.head;
		LONG k;

		if (chunk > out_ring.count)
			chunk = out_ring.count;
		k = send(sock, out_ring.buf + out_ring.head, chunk, 0);
		if (k > 0) {
			out_ring.head = (out_ring.head + (ULONG)k) & (out_ring.size - 1);
			out_ring.count -= (ULONG)k;
			bytes_sent += (ULONG)k;
			flush_progress_at = now_secs();
			continue;
		}
		if (k < 0) {
			LONG e = Errno();

			if (e != EWOULDBLOCK && e != EINTR)
				go_gone();
		}
		break;
	}
}

/* Moves queued WRITE data to the socket until the queue is empty or the
 * socket takes no more. A WRITE larger than the output buffer is taken a
 * buffer at a time; without this loop each further piece would wait for
 * the next wake-up of the main loop (up to a second), which made large
 * writes - SFTP replies of 32 KB - crawl. The pass limit keeps the other
 * event sources from waiting too long. */
static void move_output(void)
{
	int pass;

	for (pass = 0; pass < 64; pass++) {
		ULONG before = bytes_sent;

		serve_writes();
		pump_send();
		if (list_empty(&writeq) || bytes_sent == before)
			break;
	}
}

/* ---- input (from the socket) -------------------------------------------- */

/* Reads what the socket has, without blocking, into the input buffer.
 * recv() == 0 is the client's EOF on its input only: output keeps flowing.
 * A receive error ends the input the same way. */
static void pump_recv(void)
{
	ULONG tail, space;
	LONG k;

	if (sock < 0 || gone || input_eof || finishing)
		return;
	space = ring_free(&in_ring);
	if (space == 0)
		return;
	tail = (in_ring.head + in_ring.count) & (in_ring.size - 1);
	if (space > in_ring.size - tail)
		space = in_ring.size - tail;
	k = recv(sock, in_ring.buf + tail, space, 0);
	if (k > 0) {
		in_ring.count += (ULONG)k;
	} else if (k == 0) {
		input_eof = 1;
	} else {
		LONG e = Errno();

		if (e != EWOULDBLOCK && e != EINTR)
			input_eof = 1;
	}
}

/* Moves the edited line to the reader's buffer. */
static void push_line(void)
{
	ULONG i;

	for (i = 0; i < line_len; i++)
		ring_put(&ready_ring, line[i]);
	line_len = 0;
}

/* One input byte in cooked mode (terminal, not raw): line editing with
 * local echo. CR and CR LF become LF. */
static void cooked_byte(UBYTE c)
{
	if (last_in_cr) {
		last_in_cr = 0;
		if (c == CH_LF)
			return;		/* the LF of CR LF */
	}
	if (c == CH_CR) {
		last_in_cr = 1;
		c = CH_LF;
	}
	switch (c) {
	case CH_LF:
		line[line_len++] = CH_LF;
		push_line();
		echo("\r\n", 2);
		break;
	case CH_BS:
	case CH_DEL:
		if (line_len > 0) {
			line_len--;
			echo("\b \b", 3);
		}
		break;
	case CH_CTRL_U:
		while (line_len > 0) {
			line_len--;
			echo("\b \b", 3);
		}
		break;
	case CH_CTRL_BSL:
		/* End of file: a non-empty line is handed over without LF; on
		 * an empty line the next READ returns 0. */
		if (line_len > 0)
			push_line();
		else
			eof_mark = 1;
		break;
	default:
		if (line_len < LINE_MAX - 1) {
			line[line_len++] = c;
			echo((const char *)&c, 1);
		}
		break;
	}
}

/* Puts the answers to Amiga-private sequences into the reader's input, as
 * the console device would (console.doc; AmigaOS wiki, Console Device):
 *   window bounds report  <CSI>1;1;<rows>;<columns> r
 *   window resized        <CSI>12;0;0;0;0;0;0;0|
 * The size is the one the client sent (pty-req, window-change), 24 rows
 * by 80 columns if it sent none. */
/* Puts the decimal digits of v (0 or more) into the reader's buffer. */
static void put_number(LONG v)
{
	char d[12];
	int n = 0;

	do {
		d[n++] = (char)('0' + v % 10);
		v /= 10;
	} while (v > 0 && n < (int)sizeof(d));
	while (n > 0)
		ring_put(&ready_ring, (UBYTE)d[--n]);
}

/* Puts a string into the reader's buffer. */
static void put_string(const char *s)
{
	while (*s != '\0')
		ring_put(&ready_ring, (UBYTE)*s++);
}

/* Room a report needs: CSI, two numbers of at most 10 digits, text. */
#define REPORT_ROOM 40

static void deliver_reports(void)
{
	LONG cols, rows;

	if (resize_pending && ring_free(&ready_ring) >= REPORT_ROOM) {
		ring_put(&ready_ring, CH_CSI);
		put_number(EVENT_RESIZE);
		put_string(";0;0;0;0;0;0;0|");
		resize_pending = 0;
	}
	if (wsr_pending && ring_free(&ready_ring) >= REPORT_ROOM) {
		Forbid();
		cols = blk->cols;
		rows = blk->rows;
		Permit();
		if (cols <= 0)
			cols = 80;
		if (rows <= 0)
			rows = 24;
		ring_put(&ready_ring, CH_CSI);
		put_string("1;1;");
		put_number(rows);
		ring_put(&ready_ring, ';');
		put_number(cols);
		put_string(" r");
		wsr_pending = 0;
	}
}

/* Processes buffered input bytes as far as the reader's buffer and the echo
 * space allow; the rest stays buffered until there is room. */
static void process_input(void)
{
	while (in_ring.count > 0 && !eof_mark) {
		UBYTE c;

		if (!pty || raw) {
			/* A line half typed before the switch to raw mode
			 * goes to the reader first. */
			if (line_len > 0) {
				if (ring_free(&ready_ring) < line_len)
					break;
				push_line();
			}
			if (ring_free(&ready_ring) < 1)
				break;
		} else {
			if (ring_free(&ready_ring) < LINE_MAX)
				break;
			if (output_open()
			    && ring_free(&out_ring) < ECHO_RESERVE)
				break;
		}
		c = ring_get(&in_ring);
		if (!pty) {
			ring_put(&ready_ring, c);
			continue;
		}
		/* Ctrl-C/D/E/F: break signal to the reader. Cooked mode
		 * swallows the byte; raw mode passes it on as well. */
		if (c >= 3 && c <= 6) {
			send_break((ULONG)SIGBREAKF_CTRL_C << (c - 3));
			if (!raw)
				continue;
		}
		if (raw)
			ring_put(&ready_ring, c);
		else
			cooked_byte(c);
	}
}

/* Hands up to len ready bytes to a reader. In cooked mode a READ gets at
 * most one line. */
static LONG deliver(UBYTE *buf, LONG len)
{
	LONG n = 0;

	while (n < len && ready_ring.count > 0) {
		UBYTE c = ring_get(&ready_ring);

		buf[n++] = c;
		if (pty && !raw && c == CH_LF)
			break;
	}
	return n;
}

/* No more input will come: the client sent EOF, cannot be reached, or the
 * session is over. */
static int input_ended(void)
{
	return input_eof || gone || finishing || finished;
}

/* A READ would return at once (data or end of file). */
static int input_available(void)
{
	return ready_ring.count > 0 || eof_mark || input_ended();
}

/* Answers queued READ and WAIT_CHAR packets, oldest first, as far as the
 * input allows. */
static void serve_reads(void)
{
	while (!list_empty(&readq)) {
		struct Message *msg = (struct Message *)readq.lh_Head;
		struct DosPacket *pkt = packet_of(msg);

		if (pkt->dp_Type == ACTION_WAIT_CHAR) {
			if (!input_available())
				break;
			Remove(&msg->mn_Node);
			stop_timer(pkt);
			reply_packet(pkt, DOSTRUE, 0);
			continue;
		}
		if (ready_ring.count > 0) {
			Remove(&msg->mn_Node);
			reply_packet(pkt, deliver((UBYTE *)pkt->dp_Arg2,
				pkt->dp_Arg3), 0);
			continue;
		}
		if (eof_mark) {
			eof_mark = 0;
			Remove(&msg->mn_Node);
			reply_packet(pkt, 0, 0);
			continue;
		}
		if (input_ended()) {
			/* End of file. When the client is gone, IoErr() reads
			 * ERROR_BREAK, so a shell sees the session end. */
			Remove(&msg->mn_Node);
			reply_packet(pkt, 0, gone ? ERROR_BREAK : 0);
			continue;
		}
		break;
	}
}

/* The client cannot be reached (send() failed) or the server freed the
 * child: the reader gets CTRL_C (unless the session is already over),
 * output is discarded and every READ gets end of file from now on. */
static void go_gone(void)
{
	if (gone)
		return;
	gone = 1;
	gone_at = now_secs();
	if (!finishing && !finished)
		send_break(SIGBREAKF_CTRL_C);
	ring_clear(&out_ring);
	discard_writes();
	serve_reads();
}

/* ---- packet dispatch ---------------------------------------------------- */

static void handle_packet(struct Message *msg)
{
	struct DosPacket *pkt = packet_of(msg);

	switch (pkt->dp_Type) {
	case ACTION_FINDINPUT:
	case ACTION_FINDOUTPUT:
	case ACTION_FINDUPDATE: {
		/* Open("*") by a process whose console task is this port.
		 * dp_Arg1 is a BPTR to the new FileHandle. No new handles once
		 * the session is over. */
		struct FileHandle *fh = BADDR(pkt->dp_Arg1);

		if (finishing || finished) {
			reply_packet(pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
			break;
		}
		fh->fh_Type = hport;
		fh->fh_Port = hport;	/* nonzero: interactive */
		fh->fh_Arg1 = 1;
		opens++;
		saw_find = 1;
		reply_packet(pkt, DOSTRUE, 0);
		break;
	}

	case ACTION_END:
		opens--;
		reply_packet(pkt, DOSTRUE, 0);
		break;

	case ACTION_READ: {
		/* dp_Arg2 buffer, dp_Arg3 length; dp_Res1 the bytes read. */
		struct Task *t = packet_task(pkt);

		if (t != NULL)
			reader_task = t;
		if (pkt->dp_Arg3 <= 0) {
			reply_packet(pkt, 0, 0);
			break;
		}
		pkt->dp_Res2 = 0;
		AddTail(&readq, &msg->mn_Node);
		serve_reads();
		break;
	}

	case ACTION_WAIT_CHAR: {
		/* dp_Arg1 is the timeout in microseconds (WaitForChar()). */
		struct Task *t = packet_task(pkt);
		struct timerequest *tr;
		ULONG us = (ULONG)pkt->dp_Arg1;

		if (t != NULL)
			reader_task = t;
		pkt->dp_Res2 = 0;
		if (input_available() && list_empty(&readq)) {
			reply_packet(pkt, DOSTRUE, 0);
			break;
		}
		if (pkt->dp_Arg1 <= 0) {
			reply_packet(pkt, DOSFALSE, 0);
			break;
		}
		tr = (struct timerequest *)CreateIORequest(tport,
			sizeof(struct timerequest));
		if (tr == NULL || OpenDevice((CONST_STRPTR)TIMERNAME,
				UNIT_MICROHZ, (struct IORequest *)tr, 0) != 0) {
			if (tr != NULL)
				DeleteIORequest((struct IORequest *)tr);
			reply_packet(pkt, DOSFALSE, ERROR_NO_FREE_STORE);
			break;
		}
		tr->tr_node.io_Command = TR_ADDREQUEST;
		tr->tr_time.tv_secs = us / 1000000UL;
		tr->tr_time.tv_micro = us % 1000000UL;
		SendIO((struct IORequest *)tr);
		pkt->dp_Res2 = (LONG)tr;
		AddTail(&readq, &msg->mn_Node);
		break;
	}

	case ACTION_WRITE: {
		/* dp_Arg2 buffer, dp_Arg3 length. Answered once buffered;
		 * dp_Res1 counts the bytes taken so far. */
		struct Task *t = packet_task(pkt);

		if (t != NULL)
			writer_task = t;
		if (pkt->dp_Arg3 <= 0) {
			reply_packet(pkt, 0, 0);
			break;
		}
		if (!output_open() || finishing || finished) {
			reply_packet(pkt, pkt->dp_Arg3, 0);	/* discarded */
			break;
		}
		pkt->dp_Res1 = 0;
		AddTail(&writeq, &msg->mn_Node);
		serve_writes();
		break;
	}

	case ACTION_SCREEN_MODE:
		/* dp_Arg1: 0 line mode, 1 (or DOSTRUE) raw mode; 2 is the V47
		 * "medium" mode, which still buffers lines (dos.doc, SetMode).
		 * Without a terminal the mode changes nothing. */
		if (pty)
			raw = (pkt->dp_Arg1 != 0 && pkt->dp_Arg1 != 2);
		reply_packet(pkt, DOSTRUE, 0);
		break;

	case ACTION_CHANGE_SIGNAL: {
		/* dp_Arg2: the port whose task gets break signals from now
		 * on. */
		struct MsgPort *p = (struct MsgPort *)pkt->dp_Arg2;

		if (p != NULL && (p->mp_Flags & PF_ACTION) == PA_SIGNAL
		    && p->mp_SigTask != NULL)
			reader_task = (struct Task *)p->mp_SigTask;
		reply_packet(pkt, DOSTRUE, 0);
		break;
	}

	case ACTION_DISK_INFO: {
		/* dp_Arg1 is a BPTR to an InfoData (callers pass
		 * MKBADDR(id)). A console has no disk; id_VolumeNode stays 0,
		 * which tells callers that look for a window there that there
		 * is none. */
		struct InfoData *id = BADDR(pkt->dp_Arg1);

		if (id != NULL) {
			id->id_NumSoftErrors = 0;
			id->id_UnitNumber = 0;
			id->id_DiskState = 0;
			id->id_NumBlocks = 0;
			id->id_NumBlocksUsed = 0;
			id->id_BytesPerBlock = 0;
			id->id_DiskType = ID_NO_DISK_PRESENT;
			id->id_VolumeNode = 0;
			id->id_InUse = 0;
		}
		reply_packet(pkt, DOSTRUE, 0);
		break;
	}

	case ACTION_IS_FILESYSTEM:
		reply_packet(pkt, DOSFALSE, 0);
		break;

	case ACTION_SEEK:
		/* A console cannot seek: Seek() returns -1. */
		reply_packet(pkt, -1, ERROR_OBJECT_WRONG_TYPE);
		break;

	default:
		reply_packet(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
		break;
	}
}

/* Timer replies: the WAIT_CHAR that owns the request timed out. */
static void handle_timers(void)
{
	struct timerequest *tr;

	while ((tr = (struct timerequest *)GetMsg(tport)) != NULL) {
		struct Node *n;

		for (n = readq.lh_Head; n->ln_Succ != NULL; n = n->ln_Succ) {
			struct DosPacket *pkt = packet_of((struct Message *)n);

			if (pkt->dp_Type == ACTION_WAIT_CHAR
			    && pkt->dp_Res2 == (LONG)tr) {
				Remove(n);
				pkt->dp_Res2 = 0;
				reply_packet(pkt, DOSFALSE, 0);
				break;
			}
		}
		CloseDevice((struct IORequest *)tr);
		DeleteIORequest((struct IORequest *)tr);
	}
}

/* ---- the helper process ------------------------------------------------- */

/* Runs as its own process (NP_Entry). It sets the home directory, runs the
 * shell or command with SystemTagList() and blocks there until it returns,
 * then reports the return code to the handler.
 *
 * SystemTagList() facts used here (dos.doc, SystemTagList):
 *  - When it waits for completion it does not close SYS_Input/SYS_Output;
 *    the caller closes them after it returns.
 *  - V45 and before could release SYS_Input in some error cases (BUGS), so
 *    on -1 the handles are not closed here: leaking two FileHandles is
 *    harmless, closing one twice is not.
 *  - NP_Cli and NP_HomeDir are filtered out of the tags it passes to
 *    CreateNewProc(); NP_ConsoleTask, NP_StackSize and NP_WindowPtr pass.
 *  - The new process inherits the caller's current directory. Setting it
 *    here with CurrentDir() avoids NP_CurrentDir, whose ownership on
 *    failure differs between V45 and V47 (BUGS).
 *  - The NEWSHELL command itself runs the new shell asynchronously (NOTES),
 *    so for a shell this returns once NewShell has started it; the handler
 *    then follows the shell through its open handles.
 *
 * The final Signal() happens inside Forbid(), which ends only when this
 * process ends (or waits inside dos.library's exit code), so the handler
 * cannot unload this code while it still runs. */
/* The command line that starts an interactive shell. With the SSH start-up
 * script installed (AMIGA_SSHD_SHELL_STARTUP), NewShell runs it instead of
 * S:Shell-Startup (NEWSHELL's FROM argument); the script runs
 * S:Shell-Startup itself and adds the aliases exit and logout. Without it,
 * the shell starts as every other one does. A shared lock is the cheapest
 * way to ask whether the file exists; it is released at once. */
static const char *shell_command(void)
{
	BPTR lock = Lock((CONST_STRPTR)AMIGA_SSHD_SHELL_STARTUP, SHARED_LOCK);

	if (lock == 0)
		return "NewShell *";
	UnLock(lock);
	return "NewShell * FROM \"" AMIGA_SSHD_SHELL_STARTUP "\"";
}

/* Sets the session's environment ("NAME=VALUE" strings from the server:
 * TERM, USER, HOME, SSH_CONNECTION, ...) as local variables of this
 * process, so the shell or command started from here has them. */
static void set_environment(void)
{
	const char *e;

	if (blk->env == NULL)
		return;
	for (e = (const char *)blk->env; *e != '\0'; e += strlen(e) + 1) {
		const char *eq = strchr(e, '=');
		char name[64];
		ULONG n;

		if (eq == NULL)
			continue;
		n = (ULONG)(eq - e);
		if (n == 0 || n >= sizeof(name))
			continue;
		memcpy(name, e, n);
		name[n] = '\0';
		SetVar((CONST_STRPTR)name, (CONST_STRPTR)(eq + 1), -1,
			LV_VAR | GVF_LOCAL_ONLY);
	}
}

static LONG helper_entry(void)
{
	struct TagItem tags[7];
	BPTR lock = 0, olddir = 0;
	LONG rc;

	set_environment();
	if (blk->home != NULL) {
		lock = Lock(blk->home, SHARED_LOCK);
		if (lock != 0)
			olddir = CurrentDir(lock);
	}

	tags[0].ti_Tag = SYS_Input;
	tags[0].ti_Data = (ULONG)h_in;
	tags[1].ti_Tag = SYS_Output;
	tags[1].ti_Data = (ULONG)h_out;
	/* Text typed by the user goes to the user shell (dos.doc). */
	tags[2].ti_Tag = SYS_UserShell;
	tags[2].ti_Data = TRUE;
	tags[3].ti_Tag = NP_ConsoleTask;
	tags[3].ti_Data = (ULONG)hport;
	tags[4].ti_Tag = NP_StackSize;
	tags[4].ti_Data = SHELL_STACK;
	/* No requesters: nobody sits at the Amiga's screen to answer one,
	 * and a "please insert volume" requester would block for ever. */
	tags[5].ti_Tag = NP_WindowPtr;
	tags[5].ti_Data = (ULONG)-1;
	tags[6].ti_Tag = TAG_END;
	tags[6].ti_Data = 0;

	rc = SystemTagList(blk->command != NULL ? (CONST_STRPTR)blk->command
		: (CONST_STRPTR)shell_command(), tags);
	if (rc != -1) {
		Close(h_in);
		Close(h_out);
	}
	if (lock != 0) {
		CurrentDir(olddir);
		UnLock(lock);
	}

	Forbid();
	h_rc = rc;
	h_closed = (rc != -1);
	h_done = 1;
	Signal(self, 1UL << helper_sigbit);
	return 0;
}

/* A FileHandle on the handler port, as a BPTR. AllocDosObject() returns a
 * plain pointer (dos_protos.h: APTR), so MKBADDR() makes the BPTR. */
static BPTR make_handle(void)
{
	struct FileHandle *fh = AllocDosObject(DOS_FILEHANDLE, NULL);

	if (fh == NULL)
		return 0;
	fh->fh_Type = hport;
	fh->fh_Port = hport;		/* nonzero: interactive */
	fh->fh_Arg1 = 1;
	return MKBADDR(fh);
}

/* Creates the two handles and starts the helper. On failure the session
 * ends at once with RETURN_FAIL. */
static void start_helper(void)
{
	struct TagItem tags[5];

	h_in = make_handle();
	h_out = make_handle();
	if (h_in != 0 && h_out != 0) {
		opens = 2;
		tags[0].ti_Tag = NP_Entry;
		tags[0].ti_Data = (ULONG)helper_entry;
		tags[1].ti_Tag = NP_StackSize;
		tags[1].ti_Data = HELPER_STACK;
		tags[2].ti_Tag = NP_Name;
		tags[2].ti_Data = (ULONG)"sshd-console helper";
		tags[3].ti_Tag = NP_WindowPtr;
		tags[3].ti_Data = (ULONG)-1;
		tags[4].ti_Tag = TAG_END;
		tags[4].ti_Data = 0;
		if (CreateNewProc(tags) != NULL) {
			helper_started = 1;
			return;
		}
		opens = 0;
	}
	if (h_in != 0)
		FreeDosObject(DOS_FILEHANDLE, BADDR(h_in));
	if (h_out != 0)
		FreeDosObject(DOS_FILEHANDLE, BADDR(h_out));
	h_in = h_out = 0;
	helper_done = 1;
	helper_done_at = now_secs();
	helper_rc = RETURN_FAIL;
}

/* Picks up the helper's result once it has signalled. */
static void check_helper(void)
{
	LONG done, rc, closed;

	if (helper_done)
		return;
	Forbid();
	done = h_done;
	rc = h_rc;
	closed = h_closed;
	Permit();
	if (!done)
		return;
	helper_done = 1;
	helper_done_at = now_secs();
	helper_rc = rc;
	/* Handles the helper did not close (SystemTagList() returned -1)
	 * will never send ACTION_END. */
	if (!closed)
		opens -= 2;
}

/* ---- the control block -------------------------------------------------- */

/* Takes break requests from the server and notices when it lets go of the
 * child. */
static void poll_block(void)
{
	ULONG bits;
	LONG attached;

	LONG resized;

	Forbid();
	bits = blk->break_bits;
	blk->break_bits = 0;
	attached = blk->attached;
	resized = blk->winsize_changed;
	blk->winsize_changed = 0;
	Permit();
	if (resized && resize_events)
		resize_pending = 1;	/* the program asked to hear of it */
	if (bits != 0)
		send_break(bits);
	if (!attached)
		go_gone();	/* nobody reads the output any more */
}

/* Publishes the return code: under Forbid(), and the server is signalled
 * only while it is still attached, so a server that has freed the child
 * (or exited) is never signalled. */
static void publish_rc(LONG rc)
{
	Forbid();
	blk->rc = rc;
	blk->finished = 1;
	if (blk->attached && blk->server_task != NULL
	    && blk->server_sigbit >= 0)
		Signal(blk->server_task, 1UL << blk->server_sigbit);
	Permit();
}

/* Drops this process's reference to the block; the last owner frees it. */
static void release_block(void)
{
	int last;

	Forbid();
	blk->handler_task = NULL;
	blk->handler_sigbit = -1;
	last = (--blk->refcount == 0);
	Permit();
	if (last)
		FreeVec(blk);
	blk = NULL;
}

/* ---- session end -------------------------------------------------------- */

/* True once the shell or command is over and the result can be sent:
 *  - exec: the helper returned (the command's return code is final);
 *  - shell: the helper returned and every handle on the port is closed,
 *    after NewShell had time to open "*";
 *  - in either case, the client has been gone for DRAIN_SECS after the
 *    helper returned. */
static int session_over(ULONG now)
{
	if (!helper_done)
		return 0;
	if (blk->command != NULL)
		return 1;
	if (opens <= 0 && (saw_find || helper_rc != 0
			   || now - helper_done_at >= SPAWN_SECS))
		return 1;
	if (gone && now - gone_at >= DRAIN_SECS
	    && now - helper_done_at >= DRAIN_SECS)
		return 1;
	return 0;
}

/* Ends the output: shutdown(SHUT_WR) so the server reads EOF, then close
 * the socket, then publish the return code. */
static void close_channel(void)
{
	if (sock >= 0) {
		if (!gone)
			shutdown(sock, SHUT_WR);
		CloseSocket(sock);
		sock = -1;
	}
	ring_clear(&out_ring);
	discard_writes();
	serve_reads();
	publish_rc(helper_rc);
	finished = 1;
	finished_at = now_secs();
}

/* The last answer to a packet that arrives while the port is being shut
 * down (under Forbid(): nothing here may wait). */
static void final_reply(struct Message *msg)
{
	struct DosPacket *pkt = packet_of(msg);

	switch (pkt->dp_Type) {
	case ACTION_READ:
		reply_packet(pkt, 0, ERROR_BREAK);
		break;
	case ACTION_WRITE:
		reply_packet(pkt, pkt->dp_Arg3, 0);
		break;
	case ACTION_END:
		reply_packet(pkt, DOSTRUE, 0);
		break;
	case ACTION_FINDINPUT:
	case ACTION_FINDOUTPUT:
	case ACTION_FINDUPDATE:
		reply_packet(pkt, DOSFALSE, ERROR_OBJECT_NOT_FOUND);
		break;
	case ACTION_WAIT_CHAR:
	case ACTION_IS_FILESYSTEM:
		reply_packet(pkt, DOSFALSE, 0);
		break;
	default:
		reply_packet(pkt, DOSFALSE, ERROR_ACTION_NOT_KNOWN);
		break;
	}
}

/* Answers everything still queued and retires the handler port. If the
 * helper ever ran, processes started from the session (a "run" job, for
 * example) may still hold this port as their console and send to it after
 * this program is gone. The port's memory is then kept, set to PA_IGNORE
 * with no task: a late packet is queued and never answered, which blocks
 * that sender instead of writing into freed memory. */
static void close_port(void)
{
	struct Message *msg;

	while ((msg = (struct Message *)RemHead(&readq)) != NULL) {
		struct DosPacket *pkt = packet_of(msg);

		if (pkt->dp_Type == ACTION_WAIT_CHAR) {
			stop_timer(pkt);
			reply_packet(pkt, DOSFALSE, 0);
		} else {
			reply_packet(pkt, 0, ERROR_BREAK);
		}
	}
	discard_writes();

	if (!helper_started) {
		while ((msg = GetMsg(hport)) != NULL)
			final_reply(msg);
		DeleteMsgPort(hport);
		hport = NULL;
		return;
	}
	Forbid();
	hport->mp_Flags = PA_IGNORE;
	hport->mp_SigTask = NULL;
	while ((msg = GetMsg(hport)) != NULL)
		final_reply(msg);
	Permit();
	FreeSignal(hport->mp_SigBit);
	hport = NULL;
}

/* ---- startup ------------------------------------------------------------ */

/* Parses a decimal number (optionally negative) that fits a LONG. Returns
 * 0 on success. The magnitude is built unsigned, so no step overflows. */
static int parse_dec(const char *s, LONG *out)
{
	ULONG v = 0, limit;
	int neg = 0;

	if (*s == '-') {
		neg = 1;
		s++;
	}
	if (*s == '\0')
		return -1;
	limit = neg ? 0x80000000UL : 0x7fffffffUL;
	for (; *s != '\0'; s++) {
		ULONG d;

		if (*s < '0' || *s > '9')
			return -1;
		d = (ULONG)(*s - '0');
		if (v > (limit - d) / 10)
			return -1;
		v = v * 10 + d;
	}
	if (neg)
		*out = v == 0x80000000UL ? (LONG)(-0x7fffffffL - 1) : -(LONG)v;
	else
		*out = (LONG)v;
	return 0;
}

/* Parses a hexadecimal number. Returns 0 on success. */
static int parse_hex(const char *s, ULONG *out)
{
	ULONG v = 0;
	int digits = 0;

	for (; *s != '\0'; s++, digits++) {
		int d;

		if (*s >= '0' && *s <= '9')
			d = *s - '0';
		else if (*s >= 'a' && *s <= 'f')
			d = *s - 'a' + 10;
		else if (*s >= 'A' && *s <= 'F')
			d = *s - 'A' + 10;
		else
			return -1;
		v = (v << 4) | (ULONG)d;
	}
	if (digits == 0 || digits > 8)
		return -1;
	*out = v;
	return 0;
}

/* Checks that addr points at a control block of this build: in RAM that
 * exec manages, longword aligned, with the right magic and size. */
static struct amiga_child *check_block(ULONG addr)
{
	struct amiga_child *b = (struct amiga_child *)addr;

	if (addr == 0 || (addr & 3) != 0 || TypeOfMem((APTR)addr) == 0
	    || TypeOfMem((APTR)(addr + sizeof(*b) - 1)) == 0)
		return NULL;
	if (b->magic != AMIGA_CHILD_MAGIC || b->size != sizeof(*b))
		return NULL;
	return b;
}

/* ---- main --------------------------------------------------------------- */

int main(int argc, char **argv)
{
	struct TagItem sbtags[2];
	LONG id;
	ULONG addr;

	self = FindTask(NULL);
	list_init(&readq);
	list_init(&writeq);

	if (argc < 3 || parse_dec(argv[1], &id) != 0
	    || parse_hex(argv[2], &addr) != 0)
		return RETURN_FAIL;
	blk = check_block(addr);
	if (blk == NULL)
		return RETURN_FAIL;	/* not a block of this build: untouched */
	pty = blk->pty != 0;

	/* The socket first, so that it is never left unclaimed in the
	 * stack. */
	SocketBase = OpenLibrary((CONST_STRPTR)"bsdsocket.library", 4);
	if (SocketBase != NULL) {
		/* No break signal may interrupt WaitSelect() here: Ctrl-C
		 * belongs to the shell, never to the handler. */
		sbtags[0].ti_Tag = SBTM_SETVAL(SBTC_BREAKMASK);
		sbtags[0].ti_Data = 0;
		sbtags[1].ti_Tag = TAG_END;
		sbtags[1].ti_Data = 0;
		SocketBaseTagList(sbtags);
		sock = ObtainSocket(id, AF_INET, SOCK_STREAM, 0);
		if (sock >= FD_SETSIZE) {
			/* Could not be passed to WaitSelect() in an fd_set. */
			CloseSocket(sock);
			sock = -1;
		}
		if (sock >= 0) {
			LONG one = 1;

			IoctlSocket(sock, FIONBIO, (APTR)&one);
		}
	}

	hport = CreateMsgPort();
	tport = CreateMsgPort();
	ctl_sigbit = AllocSignal(-1);
	helper_sigbit = AllocSignal(-1);

	/* Register with the block, then take requests made before start. */
	Forbid();
	blk->handler_sigbit = ctl_sigbit;
	blk->handler_task = ctl_sigbit >= 0 ? self : NULL;
	Permit();

	if (sock < 0 || hport == NULL || tport == NULL || ctl_sigbit < 0
	    || helper_sigbit < 0) {
		helper_done = 1;
		helper_done_at = now_secs();
		helper_rc = RETURN_FAIL;
		finishing = 1;
	} else {
		start_helper();
	}

	while (!finished || (opens > 0
			     && now_secs() - finished_at < DRAIN_SECS)) {
		fd_set rd, wr;
		struct timeval tv;
		ULONG mask = 0, now;
		int want_rd, want_wr;
		LONG n;

		if (hport != NULL)
			mask |= 1UL << hport->mp_SigBit;
		if (tport != NULL)
			mask |= 1UL << tport->mp_SigBit;
		if (ctl_sigbit >= 0)
			mask |= 1UL << ctl_sigbit;
		if (helper_sigbit >= 0)
			mask |= 1UL << helper_sigbit;

		want_rd = sock >= 0 && !gone && !input_eof && !finishing
			&& ring_free(&in_ring) > 0;
		want_wr = sock >= 0 && !gone && out_ring.count > 0;
		FD_ZERO(&rd);
		FD_ZERO(&wr);
		if (want_rd)
			FD_SET(sock, &rd);
		if (want_wr)
			FD_SET(sock, &wr);
		/* One second at most, for the time limits below. Without
		 * bsdsocket.library there is nothing to wait for: the helper
		 * was never started, and this first pass ends the session. */
		tv.tv_sec = 1;
		tv.tv_usec = 0;
		if (!list_empty(&writeq) && output_open()
		    && out_ring.count < out_ring.size)
			tv.tv_sec = 0;	/* more WRITE data to take: poll only */
		n = 0;
		if (SocketBase != NULL) {
			n = WaitSelect(want_rd || want_wr ? sock + 1 : 0,
				want_rd ? &rd : NULL, want_wr ? &wr : NULL,
				NULL, &tv, &mask);
			if (n < 0 && Errno() != EINTR && (want_rd || want_wr))
				go_gone();	/* the socket itself failed */
		}

		/* Every source is polled each time round; the signal mask
		 * only ends the wait early. */
		if (blk != NULL)
			poll_block();
		if (n > 0 && want_rd && FD_ISSET(sock, &rd))
			pump_recv();
		if (tport != NULL)
			handle_timers();
		if (hport != NULL) {
			struct Message *msg;

			while ((msg = GetMsg(hport)) != NULL)
				handle_packet(msg);
		}
		check_helper();
		process_input();
		move_output();
		deliver_reports();
		serve_reads();
		move_output();

		now = now_secs();
		if (!finishing && session_over(now)) {
			finishing = 1;
			flush_progress_at = now;
			serve_reads();	/* pending READs now get EOF */
		}
		if (finishing && !finished) {
			serve_writes();
			pump_send();
			if ((out_ring.count == 0 && list_empty(&writeq))
			    || !output_open()
			    || now - flush_progress_at >= FLUSH_SECS)
				close_channel();
		}
	}

	if (hport != NULL)
		close_port();
	if (tport != NULL)
		DeleteMsgPort(tport);
	if (sock >= 0)
		CloseSocket(sock);
	if (SocketBase != NULL)
		CloseLibrary(SocketBase);
	release_block();
	if (helper_sigbit >= 0)
		FreeSignal(helper_sigbit);
	if (ctl_sigbit >= 0)
		FreeSignal(ctl_sigbit);
	return RETURN_OK;
}
