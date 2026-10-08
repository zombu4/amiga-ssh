/*
 * amiga_sftp.c - sftp-server: the SFTP file transfer server for AmigaOS.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Dropbear starts this program for every channel that asks for the "sftp"
 * subsystem (svr-chansession.c, SFTPSERVER_PATH in port/localoptions.h).
 * It reads SFTP requests from Input() and writes the replies to Output();
 * the console handler (port/amiga_console.c) carries both unchanged.
 *
 * Protocol: SFTP version 3, as specified in draft-ietf-secsh-filexfer-02.
 * Every packet is "uint32 length, byte type, data"; integers are big-endian,
 * strings are a uint32 length followed by the bytes.
 *
 * Paths: SFTP clients see one tree under "/". "/" itself is a virtual
 * directory that lists the mounted volumes and the assigns; "/Work/a/b" is
 * the AmigaOS path "Work:a/b". Relative paths start at the current
 * directory, which the console handler sets to the account's home
 * directory. "." and ".." are resolved in the text first, so ".." never
 * leaves "/". See docs/DESIGN.md, File transfer.
 *
 * Only dos.library is used for files and for the two streams: libnix's
 * stdio calls __chkabort(), which can end the program with exit(20) on
 * Ctrl-C in the middle of a transfer (sources/nix/stdio/read.c,
 * nix/signal/raise.c). Requesters are switched off, so a missing volume is
 * an error for the client, never a wait at the Amiga's screen.
 */
#include <stdio.h>
#include <string.h>

#include <exec/types.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <dos/dosextens.h>
#include <proto/exec.h>
#include <proto/dos.h>

/* Version string for the AmigaDOS Version command. */
static const char version[] __attribute__((used)) =
	"$VER: sftp-server 1.0 (08.10.2026)";

/* ---- protocol constants (draft-ietf-secsh-filexfer-02) ----------------- */

#define SSH_FXP_INIT            1
#define SSH_FXP_VERSION         2
#define SSH_FXP_OPEN            3
#define SSH_FXP_CLOSE           4
#define SSH_FXP_READ            5
#define SSH_FXP_WRITE           6
#define SSH_FXP_LSTAT           7
#define SSH_FXP_FSTAT           8
#define SSH_FXP_SETSTAT         9
#define SSH_FXP_FSETSTAT       10
#define SSH_FXP_OPENDIR        11
#define SSH_FXP_READDIR        12
#define SSH_FXP_REMOVE         13
#define SSH_FXP_MKDIR          14
#define SSH_FXP_RMDIR          15
#define SSH_FXP_REALPATH       16
#define SSH_FXP_STAT           17
#define SSH_FXP_RENAME         18
#define SSH_FXP_READLINK       19
#define SSH_FXP_SYMLINK        20
#define SSH_FXP_STATUS        101
#define SSH_FXP_HANDLE        102
#define SSH_FXP_DATA          103
#define SSH_FXP_NAME          104
#define SSH_FXP_ATTRS         105
#define SSH_FXP_EXTENDED      200

#define SSH_FILEXFER_ATTR_SIZE         0x00000001UL
#define SSH_FILEXFER_ATTR_UIDGID       0x00000002UL
#define SSH_FILEXFER_ATTR_PERMISSIONS  0x00000004UL
#define SSH_FILEXFER_ATTR_ACMODTIME    0x00000008UL
#define SSH_FILEXFER_ATTR_EXTENDED     0x80000000UL

#define SSH_FXF_READ    0x00000001UL
#define SSH_FXF_WRITE   0x00000002UL
#define SSH_FXF_APPEND  0x00000004UL
#define SSH_FXF_CREAT   0x00000008UL
#define SSH_FXF_TRUNC   0x00000010UL
#define SSH_FXF_EXCL    0x00000020UL

#define SSH_FX_OK                 0
#define SSH_FX_EOF                1
#define SSH_FX_NO_SUCH_FILE       2
#define SSH_FX_PERMISSION_DENIED  3
#define SSH_FX_FAILURE            4
#define SSH_FX_BAD_MESSAGE        5
#define SSH_FX_OP_UNSUPPORTED     8

/* The protocol version this server speaks. */
#define SFTP_VERSION 3

/* ---- limits ------------------------------------------------------------- */

/* Largest request accepted, the same limit OpenSSH's sftp-server uses
 * (256 KB); a client asking for more is broken or hostile. */
#define MAX_PACKET (256UL * 1024UL)

/* Largest READ answered at once; clients ask for 32 KB by default and
 * accept shorter replies. */
#define MAX_READ (64UL * 1024UL)

/* Open handles at once. */
#define MAX_HANDLES 64

/* Names sent in one READDIR reply. */
#define DIR_BATCH 64

/* Longest SFTP or AmigaOS path handled. */
#define PATH_MAX_LEN 1024

/* Entries of the virtual "/" directory. */
#define MAX_ROOT_ENTRIES 128
#define ROOT_NAME_LEN 64

/* Unix file type and permission bits, as SFTP clients expect them. */
#define S_IFDIR_BITS 0040000UL
#define S_IFREG_BITS 0100000UL
#define S_IFLNK_BITS 0120000UL
#define S_IFMT_BITS  0170000UL

/* Days from 1970-01-01 to 1978-01-01, the AmigaOS epoch (8 years, two of
 * them leap years: 1972 and 1976). */
#define AMIGA_EPOCH_DAYS 2922UL

/* ---- streams and buffers ------------------------------------------------ */

static BPTR in_fh, out_fh;

/* A growing output buffer for one reply. */
struct obuf {
	UBYTE *d;
	ULONG len, cap;
	int failed;		/* out of memory: the reply is not sent */
};

/* Reads exactly n bytes from the input. Returns 1, or 0 at end of input or
 * on an error. */
static int read_full(UBYTE *p, ULONG n)
{
	while (n > 0) {
		LONG k = Read(in_fh, p, (LONG)n);

		if (k <= 0)
			return 0;
		p += k;
		n -= (ULONG)k;
	}
	return 1;
}

/* Writes n bytes to the output. Returns 1, or 0 on an error. */
static int write_full(const UBYTE *p, ULONG n)
{
	while (n > 0) {
		LONG k = Write(out_fh, (APTR)p, (LONG)n);

		if (k <= 0)
			return 0;
		p += k;
		n -= (ULONG)k;
	}
	return 1;
}

static void ob_reset(struct obuf *b)
{
	b->len = 0;
	b->failed = 0;
}

/* Makes room for n more bytes. */
static int ob_room(struct obuf *b, ULONG n)
{
	UBYTE *nd;
	ULONG ncap;

	if (b->failed)
		return 0;
	if (b->len + n <= b->cap)
		return 1;
	ncap = b->cap ? b->cap : 4096;
	while (ncap < b->len + n)
		ncap *= 2;
	nd = AllocVec(ncap, MEMF_ANY);
	if (nd == NULL) {
		b->failed = 1;
		return 0;
	}
	if (b->d != NULL) {
		CopyMem(b->d, nd, b->len);
		FreeVec(b->d);
	}
	b->d = nd;
	b->cap = ncap;
	return 1;
}

static void ob_bytes(struct obuf *b, const void *p, ULONG n)
{
	if (ob_room(b, n)) {
		CopyMem((APTR)p, b->d + b->len, n);
		b->len += n;
	}
}

static void ob_u8(struct obuf *b, ULONG v)
{
	UBYTE c = (UBYTE)v;

	ob_bytes(b, &c, 1);
}

static void ob_u32(struct obuf *b, ULONG v)
{
	UBYTE c[4];

	c[0] = (UBYTE)(v >> 24);
	c[1] = (UBYTE)(v >> 16);
	c[2] = (UBYTE)(v >> 8);
	c[3] = (UBYTE)v;
	ob_bytes(b, c, 4);
}

/* A uint64 whose high half is always 0: AmigaOS 3.x file sizes are LONG. */
static void ob_u64(struct obuf *b, ULONG low)
{
	ob_u32(b, 0);
	ob_u32(b, low);
}

static void ob_string(struct obuf *b, const void *p, ULONG n)
{
	ob_u32(b, n);
	ob_bytes(b, p, n);
}

static void ob_cstring(struct obuf *b, const char *s)
{
	ob_string(b, s, strlen(s));
}

/* Sends the buffer as one packet (length prefix plus contents). Returns 0
 * if the output failed or the reply could not be built for lack of memory;
 * either ends the server, which closes the channel, so the client reports
 * an error instead of waiting for a reply that never comes. */
static int ob_send(struct obuf *b)
{
	UBYTE len[4];

	if (b->failed)
		return 0;
	len[0] = (UBYTE)(b->len >> 24);
	len[1] = (UBYTE)(b->len >> 16);
	len[2] = (UBYTE)(b->len >> 8);
	len[3] = (UBYTE)b->len;
	return write_full(len, 4) && write_full(b->d, b->len);
}

/* ---- request parsing ---------------------------------------------------- */

/* The request being parsed: its bytes and the read position. Every get_*
 * fails (returns 0) instead of reading past the end. */
struct req {
	const UBYTE *p;
	ULONG len, pos;
};

static int get_u32(struct req *r, ULONG *v)
{
	if (r->len - r->pos < 4)
		return 0;
	*v = ((ULONG)r->p[r->pos] << 24) | ((ULONG)r->p[r->pos + 1] << 16)
		| ((ULONG)r->p[r->pos + 2] << 8) | r->p[r->pos + 3];
	r->pos += 4;
	return 1;
}

/* A uint64; *hi gets the high 32 bits so callers can refuse what a LONG
 * file position cannot hold. */
static int get_u64(struct req *r, ULONG *hi, ULONG *lo)
{
	return get_u32(r, hi) && get_u32(r, lo);
}

static int get_string(struct req *r, const UBYTE **s, ULONG *n)
{
	if (!get_u32(r, n) || r->len - r->pos < *n)
		return 0;
	*s = r->p + r->pos;
	r->pos += *n;
	return 1;
}

/* Attributes in a request. Only what AmigaOS can apply is kept. */
struct attrs {
	ULONG flags;
	ULONG size_hi, size;
	ULONG perm;
	ULONG atime, mtime;
};

static int get_attrs(struct req *r, struct attrs *a)
{
	ULONG skip, count, i;
	const UBYTE *s;

	memset(a, 0, sizeof(*a));
	if (!get_u32(r, &a->flags))
		return 0;
	if ((a->flags & SSH_FILEXFER_ATTR_SIZE)
	    && !get_u64(r, &a->size_hi, &a->size))
		return 0;
	if ((a->flags & SSH_FILEXFER_ATTR_UIDGID)
	    && !(get_u32(r, &skip) && get_u32(r, &skip)))
		return 0;
	if ((a->flags & SSH_FILEXFER_ATTR_PERMISSIONS) && !get_u32(r, &a->perm))
		return 0;
	if ((a->flags & SSH_FILEXFER_ATTR_ACMODTIME)
	    && !(get_u32(r, &a->atime) && get_u32(r, &a->mtime)))
		return 0;
	if (a->flags & SSH_FILEXFER_ATTR_EXTENDED) {
		if (!get_u32(r, &count))
			return 0;
		for (i = 0; i < count; i++) {
			if (!get_string(r, &s, &skip) || !get_string(r, &s, &skip))
				return 0;
		}
	}
	return 1;
}

/* ---- replies ------------------------------------------------------------ */

static struct obuf reply;

static const char *status_text(ULONG code)
{
	switch (code) {
	case SSH_FX_OK: return "Success";
	case SSH_FX_EOF: return "End of file";
	case SSH_FX_NO_SUCH_FILE: return "No such file";
	case SSH_FX_PERMISSION_DENIED: return "Permission denied";
	case SSH_FX_BAD_MESSAGE: return "Bad message";
	case SSH_FX_OP_UNSUPPORTED: return "Operation unsupported";
	default: return "Failure";
	}
}

static int send_status_msg(ULONG id, ULONG code, const char *msg)
{
	ob_reset(&reply);
	ob_u8(&reply, SSH_FXP_STATUS);
	ob_u32(&reply, id);
	ob_u32(&reply, code);
	ob_cstring(&reply, msg);
	ob_cstring(&reply, "");
	return ob_send(&reply);
}

static int send_status(ULONG id, ULONG code)
{
	return send_status_msg(id, code, status_text(code));
}

/* Answers with the SFTP status for the DOS error err, with the DOS error
 * text as the message. */
static int send_dos_error(ULONG id, LONG err)
{
	char text[96];
	ULONG code;

	switch (err) {
	case ERROR_OBJECT_NOT_FOUND:
	case ERROR_DIR_NOT_FOUND:
	case ERROR_DEVICE_NOT_MOUNTED:
	case ERROR_NO_DISK:
		code = SSH_FX_NO_SUCH_FILE;
		break;
	case ERROR_DELETE_PROTECTED:
	case ERROR_WRITE_PROTECTED:
	case ERROR_READ_PROTECTED:
	case ERROR_DISK_WRITE_PROTECTED:
		code = SSH_FX_PERMISSION_DENIED;
		break;
	default:
		code = SSH_FX_FAILURE;
		break;
	}
	if (err == 0 || !Fault(err, NULL, (STRPTR)text, sizeof(text)))
		return send_status(id, code);
	/* Fault() with no header writes the bare message. */
	return send_status_msg(id, code, text);
}

/* ---- paths -------------------------------------------------------------- */

/* The current directory as an SFTP path ("/Vol/dir"), set at start. */
static char home_sftp[PATH_MAX_LEN];

/* Turns an AmigaOS path from NameFromLock() ("Vol:a/b" or "Vol:") into an
 * SFTP path ("/Vol/a/b" or "/Vol"). */
static void amiga_to_sftp(const char *apath, char *out, ULONG size)
{
	const char *colon = strchr(apath, ':');
	ULONG n = 0;

	if (colon == NULL) {
		strncpy(out, "/", size);
		return;
	}
	out[n++] = '/';
	while (apath < colon && n < size - 1)
		out[n++] = *apath++;
	apath = colon + 1;
	if (*apath != '\0' && n < size - 1)
		out[n++] = '/';
	while (*apath != '\0' && n < size - 1)
		out[n++] = *apath++;
	/* no trailing '/' */
	while (n > 1 && out[n - 1] == '/')
		n--;
	out[n] = '\0';
}

/* Resolves the client's path (len bytes, not NUL-terminated) into a
 * canonical SFTP path: absolute, no ".", "..", empty components or
 * trailing '/'. Relative paths start at home_sftp. Returns 0 if the path
 * is too long, holds a NUL or a ':' in a component. */
static int resolve(const UBYTE *path, ULONG len, char *out)
{
	char work[PATH_MAX_LEN * 2];
	ULONG w = 0, i, o;
	char *comp, *next;

	for (i = 0; i < len; i++) {
		if (path[i] == '\0' || path[i] == ':')
			return 0;
	}
	if (len == 0 || path[0] != '/') {
		w = strlen(home_sftp);
		memcpy(work, home_sftp, w);
		work[w++] = '/';
	}
	if (w + len + 1 > sizeof(work))
		return 0;
	memcpy(work + w, path, len);
	work[w + len] = '\0';

	/* Walk the components, keeping a stack of the kept ones in out. */
	o = 0;
	out[0] = '\0';
	for (comp = work; comp != NULL; comp = next) {
		ULONG clen;

		next = strchr(comp, '/');
		if (next != NULL)
			*next++ = '\0';
		clen = strlen(comp);
		if (clen == 0 || strcmp(comp, ".") == 0)
			continue;
		if (strcmp(comp, "..") == 0) {
			while (o > 0 && out[o - 1] != '/')
				o--;
			if (o > 0)
				o--;	/* drop the '/' too */
			out[o] = '\0';
			continue;
		}
		if (o + 1 + clen + 1 > PATH_MAX_LEN)
			return 0;
		out[o++] = '/';
		memcpy(out + o, comp, clen);
		o += clen;
		out[o] = '\0';
	}
	if (o == 0)
		strcpy(out, "/");
	return 1;
}

/* Turns a canonical SFTP path into an AmigaOS path. Returns 0 for "/",
 * which has no AmigaOS equivalent (the virtual root). */
static int sftp_to_amiga(const char *sftp, char *out)
{
	const char *slash;
	ULONG n;

	if (strcmp(sftp, "/") == 0)
		return 0;
	sftp++;
	slash = strchr(sftp, '/');
	n = slash ? (ULONG)(slash - sftp) : strlen(sftp);
	memcpy(out, sftp, n);
	out[n++] = ':';
	if (slash != NULL)
		strcpy(out + n, slash + 1);
	else
		out[n] = '\0';
	return 1;
}

/* Resolves a path string from a request into both forms. *is_root is set
 * for "/". Returns 0 for an unusable path. */
static int request_path(const UBYTE *s, ULONG n, char *sftp, char *apath,
		int *is_root)
{
	if (!resolve(s, n, sftp))
		return 0;
	*is_root = !sftp_to_amiga(sftp, apath);
	return 1;
}

/* ---- attributes --------------------------------------------------------- */

/* Seconds since 1970 for an AmigaOS DateStamp (local time, see
 * docs/DESIGN.md). */
static ULONG ds_to_unix(const struct DateStamp *ds)
{
	return ((ULONG)ds->ds_Days + AMIGA_EPOCH_DAYS) * 86400UL
		+ (ULONG)ds->ds_Minute * 60UL + (ULONG)ds->ds_Tick / TICKS_PER_SECOND;
}

static void unix_to_ds(ULONG t, struct DateStamp *ds)
{
	ULONG days = t / 86400UL, rest = t % 86400UL;

	ds->ds_Days = days > AMIGA_EPOCH_DAYS ? (LONG)(days - AMIGA_EPOCH_DAYS) : 0;
	ds->ds_Minute = (LONG)(rest / 60UL);
	ds->ds_Tick = (LONG)(rest % 60UL) * TICKS_PER_SECOND;
}

/* What a FileInfoBlock describes. fib_DirEntryType is positive for
 * directories, but also for soft links, which may point to a file
 * (dos/dosextens.h, ST_SOFTLINK); only these three are directories. */
static int fib_is_dir(const struct FileInfoBlock *fib)
{
	LONG t = fib->fib_DirEntryType;

	return t == ST_ROOT || t == ST_USERDIR || t == ST_LINKDIR;
}

static int fib_is_softlink(const struct FileInfoBlock *fib)
{
	return fib->fib_DirEntryType == ST_SOFTLINK;
}

/* Unix permission bits for a FileInfoBlock. AmigaOS's owner bits R, W, E
 * mean "not allowed" when set; the group and other bits mean "allowed". */
static ULONG fib_perm(const struct FileInfoBlock *fib)
{
	ULONG p = (ULONG)fib->fib_Protection, m = 0;

	if (!(p & FIBF_READ))
		m |= 0400;
	if (!(p & FIBF_WRITE))
		m |= 0200;
	if (!(p & FIBF_EXECUTE))
		m |= 0100;
	if (p & FIBF_GRP_READ)
		m |= 0040;
	if (p & FIBF_GRP_WRITE)
		m |= 0020;
	if (p & FIBF_GRP_EXECUTE)
		m |= 0010;
	if (p & FIBF_OTR_READ)
		m |= 0004;
	if (p & FIBF_OTR_WRITE)
		m |= 0002;
	if (p & FIBF_OTR_EXECUTE)
		m |= 0001;
	if (fib_is_dir(fib))
		m |= S_IFDIR_BITS | 0100;	/* a directory can be entered */
	else if (fib_is_softlink(fib))
		m |= S_IFLNK_BITS;
	else
		m |= S_IFREG_BITS;
	return m;
}

/* AmigaOS protection bits for Unix permission bits, keeping the bits that
 * have no Unix meaning (delete, script, pure, archive) from old. */
static LONG perm_to_prot(ULONG perm, LONG old)
{
	LONG p = old & ~(LONG)(FIBF_READ | FIBF_WRITE | FIBF_EXECUTE
		| FIBF_GRP_READ | FIBF_GRP_WRITE | FIBF_GRP_EXECUTE
		| FIBF_OTR_READ | FIBF_OTR_WRITE | FIBF_OTR_EXECUTE);

	if (!(perm & 0400))
		p |= FIBF_READ;
	if (!(perm & 0200))
		p |= FIBF_WRITE;
	if (!(perm & 0100))
		p |= FIBF_EXECUTE;
	if (perm & 0040)
		p |= FIBF_GRP_READ;
	if (perm & 0020)
		p |= FIBF_GRP_WRITE;
	if (perm & 0010)
		p |= FIBF_GRP_EXECUTE;
	if (perm & 0004)
		p |= FIBF_OTR_READ;
	if (perm & 0002)
		p |= FIBF_OTR_WRITE;
	if (perm & 0001)
		p |= FIBF_OTR_EXECUTE;
	return p;
}

/* Writes the ATTRS for a FileInfoBlock. */
static void ob_fib_attrs(struct obuf *b, const struct FileInfoBlock *fib)
{
	ULONG t = ds_to_unix(&fib->fib_Date);

	ob_u32(b, SSH_FILEXFER_ATTR_SIZE | SSH_FILEXFER_ATTR_PERMISSIONS
		| SSH_FILEXFER_ATTR_ACMODTIME);
	ob_u64(b, fib_is_dir(fib) ? 0 : (ULONG)fib->fib_Size);
	ob_u32(b, fib_perm(fib));
	ob_u32(b, t);
	ob_u32(b, t);
}

/* ATTRS of the virtual "/" and of the entries it lists. */
static void ob_dir_attrs(struct obuf *b)
{
	ob_u32(b, SSH_FILEXFER_ATTR_PERMISSIONS);
	ob_u32(b, S_IFDIR_BITS | 0755);
}

/* Day, month and year of a time in seconds since 1970 (proleptic
 * Gregorian calendar; the algorithm counts days from 0000-03-01). */
static void civil(ULONG t, ULONG *y, ULONG *mo, ULONG *d)
{
	long z = (long)(t / 86400UL) + 719468L;
	long era = z / 146097L;
	long doe = z - era * 146097L;
	long yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
	long doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
	long mp = (5 * doy + 2) / 153;

	*d = (ULONG)(doy - (153 * mp + 2) / 5 + 1);
	*mo = (ULONG)(mp < 10 ? mp + 3 : mp - 9);
	*y = (ULONG)(yoe + era * 400 + (*mo <= 2 ? 1 : 0));
}

/* The "ls -l" style line clients show for a listing. */
static void long_name(char *out, ULONG size, const char *name,
		ULONG perm, ULONG bytes, ULONG t)
{
	static const char *const months[12] = {
		"Jan", "Feb", "Mar", "Apr", "May", "Jun",
		"Jul", "Aug", "Sep", "Oct", "Nov", "Dec"
	};
	char mode[11];
	ULONG y, mo, d;
	int i;

	mode[0] = (perm & S_IFMT_BITS) == S_IFDIR_BITS ? 'd'
		: (perm & S_IFMT_BITS) == S_IFLNK_BITS ? 'l' : '-';
	for (i = 0; i < 9; i++)
		mode[1 + i] = (perm & (0400 >> i)) ? "rwxrwxrwx"[i] : '-';
	mode[10] = '\0';
	civil(t, &y, &mo, &d);
	snprintf(out, size, "%s    1 amiga    amiga    %8lu %s %2lu %02lu:%02lu %s",
		mode, (unsigned long)bytes, months[(mo - 1) % 12],
		(unsigned long)d, (unsigned long)((t / 3600) % 24),
		(unsigned long)((t / 60) % 60), name);
	(void)y;
}

/* One NAME entry for a FileInfoBlock under the given name. */
static void ob_fib_name(struct obuf *b, const char *name,
		const struct FileInfoBlock *fib)
{
	char ln[PATH_MAX_LEN + 80];

	long_name(ln, sizeof(ln), name, fib_perm(fib),
		fib_is_dir(fib) ? 0 : (ULONG)fib->fib_Size,
		ds_to_unix(&fib->fib_Date));
	ob_cstring(b, name);
	ob_cstring(b, ln);
	ob_fib_attrs(b, fib);
}

/* Examines an AmigaOS path. Returns the FileInfoBlock (free it with
 * FreeDosObject) or NULL with IoErr() set. */
static struct FileInfoBlock *examine_path(const char *apath)
{
	struct FileInfoBlock *fib = AllocDosObject(DOS_FIB, NULL);
	BPTR lock;

	if (fib == NULL) {
		SetIoErr(ERROR_NO_FREE_STORE);
		return NULL;
	}
	lock = Lock((CONST_STRPTR)apath, SHARED_LOCK);
	if (lock == 0 || !Examine(lock, fib)) {
		LONG err = IoErr();

		if (lock != 0)
			UnLock(lock);
		FreeDosObject(DOS_FIB, fib);
		SetIoErr(err);
		return NULL;
	}
	UnLock(lock);
	return fib;
}

/* ---- handles ------------------------------------------------------------ */

#define H_FREE 0
#define H_FILE 1
#define H_DIR  2
#define H_ROOT 3

struct handle {
	int type;
	BPTR fh;			/* H_FILE */
	LONG pos;			/* H_FILE: current position */
	int append;			/* H_FILE: writes go to the end */
	BPTR lock;			/* H_DIR */
	struct FileInfoBlock *fib;	/* H_DIR: ExNext() state */
	int done;			/* H_DIR, H_ROOT: listing finished */
	char (*root)[ROOT_NAME_LEN];	/* H_ROOT: names of the virtual "/" */
	int root_count, root_next;
};

static struct handle handles[MAX_HANDLES];

/* A handle string is the 4-byte table index. */
static void ob_handle(struct obuf *b, int idx)
{
	UBYTE h[4];

	h[0] = 0;
	h[1] = 0;
	h[2] = (UBYTE)(idx >> 8);
	h[3] = (UBYTE)idx;
	ob_string(b, h, 4);
}

static int alloc_handle(void)
{
	int i;

	for (i = 0; i < MAX_HANDLES; i++) {
		if (handles[i].type == H_FREE) {
			memset(&handles[i], 0, sizeof(handles[i]));
			return i;
		}
	}
	return -1;
}

/* The handle named by a request, or NULL. */
static struct handle *get_handle(struct req *r, int *idx)
{
	const UBYTE *s;
	ULONG n, i;

	if (!get_string(r, &s, &n) || n != 4)
		return NULL;
	i = ((ULONG)s[0] << 24) | ((ULONG)s[1] << 16) | ((ULONG)s[2] << 8) | s[3];
	if (i >= MAX_HANDLES || handles[i].type == H_FREE)
		return NULL;
	if (idx != NULL)
		*idx = (int)i;
	return &handles[i];
}

static void close_handle(struct handle *h)
{
	switch (h->type) {
	case H_FILE:
		Close(h->fh);
		break;
	case H_DIR:
		FreeDosObject(DOS_FIB, h->fib);
		UnLock(h->lock);
		break;
	case H_ROOT:
		FreeVec(h->root);
		break;
	}
	h->type = H_FREE;
}

/* ---- the virtual "/" ---------------------------------------------------- */

/* Compares two names the way AmigaDOS does: ASCII letters without regard
 * to case. */
static int same_name(const char *a, const char *b)
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

/* Copies the names of the mounted volumes and the bound assigns. The DOS
 * list is locked only while names are copied: no other DOS call may happen
 * while it is held. Late and non-binding assigns are left out, as resolving
 * them could start a mount. Returns the number of names. */
static int list_root(char (*names)[ROOT_NAME_LEN])
{
	struct DosList *dl;
	ULONG flags = LDF_VOLUMES | LDF_ASSIGNS | LDF_READ;
	int n = 0;

	dl = LockDosList(flags);
	while ((dl = NextDosEntry(dl, LDF_VOLUMES | LDF_ASSIGNS)) != NULL
	       && n < MAX_ROOT_ENTRIES) {
		UBYTE *bname = BADDR(dl->dol_Name);
		int len, i, dup = 0;

		if (dl->dol_Type == DLT_VOLUME && dl->dol_Task == NULL)
			continue;	/* volume not in a drive */
		if (dl->dol_Type != DLT_VOLUME && dl->dol_Type != DLT_DIRECTORY)
			continue;	/* late or non-binding assign */
		if (bname == NULL)
			continue;
		len = bname[0];
		if (len == 0 || len >= ROOT_NAME_LEN)
			continue;
		memcpy(names[n], bname + 1, len);
		names[n][len] = '\0';
		for (i = 0; i < n; i++) {
			if (same_name(names[i], names[n]))
				dup = 1;
		}
		if (!dup)
			n++;
	}
	UnLockDosList(flags);
	return n;
}

/* ---- requests ----------------------------------------------------------- */

/* Each do_* handles one request type. It returns 0 only if the reply could
 * not be written, which ends the server. */

static int do_open(ULONG id, struct req *r)
{
	char sftp[PATH_MAX_LEN], apath[PATH_MAX_LEN];
	const UBYTE *s;
	ULONG n, pflags;
	struct attrs a;
	struct handle *h;
	int idx, root;
	LONG mode;
	BPTR fh;

	if (!get_string(r, &s, &n) || !get_u32(r, &pflags) || !get_attrs(r, &a))
		return send_status(id, SSH_FX_BAD_MESSAGE);
	if (!request_path(s, n, sftp, apath, &root) || root)
		return send_status(id, SSH_FX_FAILURE);

	if (pflags & SSH_FXF_EXCL) {
		BPTR lock = Lock((CONST_STRPTR)apath, SHARED_LOCK);

		if (lock != 0) {
			UnLock(lock);
			return send_status_msg(id, SSH_FX_FAILURE, "File exists");
		}
	}
	if (!(pflags & SSH_FXF_WRITE))
		mode = MODE_OLDFILE;
	else if (pflags & SSH_FXF_TRUNC)
		mode = MODE_NEWFILE;
	else if (pflags & SSH_FXF_CREAT)
		mode = MODE_READWRITE;	/* existing or new, not truncated */
	else
		mode = MODE_OLDFILE;	/* must exist; writing is allowed */

	idx = alloc_handle();
	if (idx < 0)
		return send_status_msg(id, SSH_FX_FAILURE, "Too many open handles");
	fh = Open((CONST_STRPTR)apath, mode);
	if (fh == 0)
		return send_dos_error(id, IoErr());
	if ((a.flags & SSH_FILEXFER_ATTR_PERMISSIONS)
	    && (mode == MODE_NEWFILE || mode == MODE_READWRITE))
		SetProtection((CONST_STRPTR)apath, perm_to_prot(a.perm, 0));

	h = &handles[idx];
	h->type = H_FILE;
	h->fh = fh;
	h->pos = 0;
	h->append = (pflags & SSH_FXF_APPEND) != 0;

	ob_reset(&reply);
	ob_u8(&reply, SSH_FXP_HANDLE);
	ob_u32(&reply, id);
	ob_handle(&reply, idx);
	return ob_send(&reply);
}

static int do_close(ULONG id, struct req *r)
{
	struct handle *h = get_handle(r, NULL);

	if (h == NULL)
		return send_status(id, SSH_FX_FAILURE);
	close_handle(h);
	return send_status(id, SSH_FX_OK);
}

/* Moves a file handle to offset unless it is already there. */
static int seek_to(struct handle *h, ULONG hi, ULONG lo)
{
	if (hi != 0 || lo > 0x7fffffffUL) {
		SetIoErr(ERROR_SEEK_ERROR);
		return 0;
	}
	if (h->pos == (LONG)lo)
		return 1;
	if (Seek(h->fh, (LONG)lo, OFFSET_BEGINNING) == -1)
		return 0;
	h->pos = (LONG)lo;
	return 1;
}

static int do_read(ULONG id, struct req *r)
{
	struct handle *h = get_handle(r, NULL);
	ULONG hi, lo, len;
	LONG got;

	if (h == NULL || h->type != H_FILE)
		return send_status(id, SSH_FX_FAILURE);
	if (!get_u64(r, &hi, &lo) || !get_u32(r, &len))
		return send_status(id, SSH_FX_BAD_MESSAGE);
	if (len > MAX_READ)
		len = MAX_READ;
	if (!seek_to(h, hi, lo))
		return send_dos_error(id, IoErr());

	ob_reset(&reply);
	ob_u8(&reply, SSH_FXP_DATA);
	ob_u32(&reply, id);
	ob_u32(&reply, 0);		/* data length, filled in below */
	if (!ob_room(&reply, len))
		return send_status_msg(id, SSH_FX_FAILURE, "Out of memory");
	got = Read(h->fh, reply.d + reply.len, (LONG)len);
	if (got < 0) {
		h->pos = -1;		/* unknown: seek next time */
		return send_dos_error(id, IoErr());
	}
	if (got == 0)
		return send_status(id, SSH_FX_EOF);
	h->pos += got;
	reply.len += (ULONG)got;
	/* the data length sits after type (1) and id (4) */
	reply.d[5] = (UBYTE)((ULONG)got >> 24);
	reply.d[6] = (UBYTE)((ULONG)got >> 16);
	reply.d[7] = (UBYTE)((ULONG)got >> 8);
	reply.d[8] = (UBYTE)got;
	return ob_send(&reply);
}

static int do_write(ULONG id, struct req *r)
{
	struct handle *h = get_handle(r, NULL);
	ULONG hi, lo, n;
	const UBYTE *data;
	LONG put;

	if (h == NULL || h->type != H_FILE)
		return send_status(id, SSH_FX_FAILURE);
	if (!get_u64(r, &hi, &lo) || !get_string(r, &data, &n))
		return send_status(id, SSH_FX_BAD_MESSAGE);
	if (h->append) {
		LONG end;

		if (Seek(h->fh, 0, OFFSET_END) == -1)
			return send_dos_error(id, IoErr());
		end = Seek(h->fh, 0, OFFSET_CURRENT);
		h->pos = end;
	} else if (!seek_to(h, hi, lo)) {
		return send_dos_error(id, IoErr());
	}
	put = Write(h->fh, (APTR)data, (LONG)n);
	if (put != (LONG)n) {
		h->pos = -1;
		return send_dos_error(id, IoErr());
	}
	h->pos += put;
	return send_status(id, SSH_FX_OK);
}

/* STAT, LSTAT (AmigaOS soft links are followed by Lock(), so both are the
 * same) and the attribute reply. */
static int do_stat(ULONG id, struct req *r)
{
	char sftp[PATH_MAX_LEN], apath[PATH_MAX_LEN];
	struct FileInfoBlock *fib;
	const UBYTE *s;
	ULONG n;
	int root;

	if (!get_string(r, &s, &n))
		return send_status(id, SSH_FX_BAD_MESSAGE);
	if (!request_path(s, n, sftp, apath, &root))
		return send_status(id, SSH_FX_NO_SUCH_FILE);
	ob_reset(&reply);
	ob_u8(&reply, SSH_FXP_ATTRS);
	ob_u32(&reply, id);
	if (root) {
		ob_dir_attrs(&reply);
		return ob_send(&reply);
	}
	fib = examine_path(apath);
	if (fib == NULL)
		return send_dos_error(id, IoErr());
	ob_fib_attrs(&reply, fib);
	FreeDosObject(DOS_FIB, fib);
	return ob_send(&reply);
}

static int do_fstat(ULONG id, struct req *r)
{
	struct handle *h = get_handle(r, NULL);
	struct FileInfoBlock *fib;

	if (h == NULL)
		return send_status(id, SSH_FX_FAILURE);
	ob_reset(&reply);
	ob_u8(&reply, SSH_FXP_ATTRS);
	ob_u32(&reply, id);
	if (h->type == H_ROOT) {
		ob_dir_attrs(&reply);
		return ob_send(&reply);
	}
	fib = AllocDosObject(DOS_FIB, NULL);
	if (fib == NULL)
		return send_dos_error(id, ERROR_NO_FREE_STORE);
	if (h->type == H_FILE ? !ExamineFH(h->fh, fib) : !Examine(h->lock, fib)) {
		LONG err = IoErr();

		FreeDosObject(DOS_FIB, fib);
		return send_dos_error(id, err);
	}
	ob_fib_attrs(&reply, fib);
	FreeDosObject(DOS_FIB, fib);
	return ob_send(&reply);
}

/* Applies attributes to the object at apath; fh is an open handle on it,
 * or 0 (then the size is set through a short-lived handle). */
static int apply_attrs(ULONG id, const char *apath, BPTR fh,
		const struct attrs *a)
{
	if (a->flags & SSH_FILEXFER_ATTR_SIZE) {
		BPTR own = 0;

		if (a->size_hi != 0 || a->size > 0x7fffffffUL)
			return send_dos_error(id, ERROR_SEEK_ERROR);
		if (fh == 0) {
			own = Open((CONST_STRPTR)apath, MODE_OLDFILE);
			if (own == 0)
				return send_dos_error(id, IoErr());
		}
		if (SetFileSize(fh ? fh : own, (LONG)a->size, OFFSET_BEGINNING) == -1) {
			LONG err = IoErr();

			if (own != 0)
				Close(own);
			return send_dos_error(id, err);
		}
		if (own != 0)
			Close(own);
	}
	if (a->flags & SSH_FILEXFER_ATTR_PERMISSIONS) {
		struct FileInfoBlock *fib = examine_path(apath);
		LONG old = fib ? fib->fib_Protection : 0;

		if (fib != NULL)
			FreeDosObject(DOS_FIB, fib);
		if (!SetProtection((CONST_STRPTR)apath, perm_to_prot(a->perm, old)))
			return send_dos_error(id, IoErr());
	}
	if (a->flags & SSH_FILEXFER_ATTR_ACMODTIME) {
		struct DateStamp ds;

		unix_to_ds(a->mtime, &ds);
		if (!SetFileDate((CONST_STRPTR)apath, &ds))
			return send_dos_error(id, IoErr());
	}
	return send_status(id, SSH_FX_OK);
}

static int do_setstat(ULONG id, struct req *r)
{
	char sftp[PATH_MAX_LEN], apath[PATH_MAX_LEN];
	const UBYTE *s;
	ULONG n;
	struct attrs a;
	int root;

	if (!get_string(r, &s, &n) || !get_attrs(r, &a))
		return send_status(id, SSH_FX_BAD_MESSAGE);
	if (!request_path(s, n, sftp, apath, &root))
		return send_status(id, SSH_FX_NO_SUCH_FILE);
	if (root)
		return send_status(id, SSH_FX_PERMISSION_DENIED);
	return apply_attrs(id, apath, 0, &a);
}

static int do_fsetstat(ULONG id, struct req *r)
{
	char apath[PATH_MAX_LEN];
	struct handle *h = get_handle(r, NULL);
	struct attrs a;

	if (h == NULL || h->type != H_FILE)
		return send_status(id, SSH_FX_FAILURE);
	if (!get_attrs(r, &a))
		return send_status(id, SSH_FX_BAD_MESSAGE);
	if (!NameFromFH(h->fh, (STRPTR)apath, sizeof(apath)))
		return send_dos_error(id, IoErr());
	return apply_attrs(id, apath, h->fh, &a);
}

static int do_opendir(ULONG id, struct req *r)
{
	char sftp[PATH_MAX_LEN], apath[PATH_MAX_LEN];
	struct handle *h;
	const UBYTE *s;
	ULONG n;
	int idx, root;

	if (!get_string(r, &s, &n))
		return send_status(id, SSH_FX_BAD_MESSAGE);
	if (!request_path(s, n, sftp, apath, &root))
		return send_status(id, SSH_FX_NO_SUCH_FILE);
	idx = alloc_handle();
	if (idx < 0)
		return send_status_msg(id, SSH_FX_FAILURE, "Too many open handles");
	h = &handles[idx];

	if (root) {
		h->root = AllocVec(sizeof(*h->root) * MAX_ROOT_ENTRIES, MEMF_ANY);
		if (h->root == NULL)
			return send_dos_error(id, ERROR_NO_FREE_STORE);
		h->root_count = list_root(h->root);
		h->root_next = 0;
		h->type = H_ROOT;
	} else {
		h->fib = AllocDosObject(DOS_FIB, NULL);
		if (h->fib == NULL)
			return send_dos_error(id, ERROR_NO_FREE_STORE);
		h->lock = Lock((CONST_STRPTR)apath, SHARED_LOCK);
		if (h->lock == 0 || !Examine(h->lock, h->fib)) {
			LONG err = IoErr();

			if (h->lock != 0)
				UnLock(h->lock);
			FreeDosObject(DOS_FIB, h->fib);
			return send_dos_error(id, err);
		}
		if (!fib_is_dir(h->fib)) {
			UnLock(h->lock);
			FreeDosObject(DOS_FIB, h->fib);
			return send_dos_error(id, ERROR_OBJECT_WRONG_TYPE);
		}
		h->type = H_DIR;
	}
	ob_reset(&reply);
	ob_u8(&reply, SSH_FXP_HANDLE);
	ob_u32(&reply, id);
	ob_handle(&reply, idx);
	return ob_send(&reply);
}

static int do_readdir(ULONG id, struct req *r)
{
	struct handle *h = get_handle(r, NULL);
	ULONG count = 0;

	if (h == NULL || (h->type != H_DIR && h->type != H_ROOT))
		return send_status(id, SSH_FX_FAILURE);
	if (h->done)
		return send_status(id, SSH_FX_EOF);

	ob_reset(&reply);
	ob_u8(&reply, SSH_FXP_NAME);
	ob_u32(&reply, id);
	ob_u32(&reply, 0);		/* count, filled in below */

	if (h->type == H_ROOT) {
		while (h->root_next < h->root_count && count < DIR_BATCH) {
			char ln[ROOT_NAME_LEN + 80];
			const char *name = h->root[h->root_next++];

			long_name(ln, sizeof(ln), name, S_IFDIR_BITS | 0755, 0, 0);
			ob_cstring(&reply, name);
			ob_cstring(&reply, ln);
			ob_dir_attrs(&reply);
			count++;
		}
		if (h->root_next >= h->root_count)
			h->done = 1;
	} else {
		while (count < DIR_BATCH) {
			if (!ExNext(h->lock, h->fib)) {
				LONG err = IoErr();

				h->done = 1;
				if (err != ERROR_NO_MORE_ENTRIES && count == 0)
					return send_dos_error(id, err);
				break;
			}
			ob_fib_name(&reply, (const char *)h->fib->fib_FileName, h->fib);
			count++;
		}
	}
	if (count == 0)
		return send_status(id, SSH_FX_EOF);
	reply.d[5] = (UBYTE)(count >> 24);
	reply.d[6] = (UBYTE)(count >> 16);
	reply.d[7] = (UBYTE)(count >> 8);
	reply.d[8] = (UBYTE)count;
	return ob_send(&reply);
}

/* REMOVE (files only) and RMDIR (directories only). */
static int do_remove(ULONG id, struct req *r, int want_dir)
{
	char sftp[PATH_MAX_LEN], apath[PATH_MAX_LEN];
	struct FileInfoBlock *fib;
	const UBYTE *s;
	ULONG n;
	int root, is_dir;

	if (!get_string(r, &s, &n))
		return send_status(id, SSH_FX_BAD_MESSAGE);
	if (!request_path(s, n, sftp, apath, &root))
		return send_status(id, SSH_FX_NO_SUCH_FILE);
	if (root || strchr(sftp + 1, '/') == NULL)
		return send_status(id, SSH_FX_PERMISSION_DENIED);	/* "/" or a volume */
	fib = examine_path(apath);
	if (fib == NULL)
		return send_dos_error(id, IoErr());
	is_dir = fib_is_dir(fib);
	FreeDosObject(DOS_FIB, fib);
	if (is_dir != want_dir)
		return send_dos_error(id, ERROR_OBJECT_WRONG_TYPE);
	if (!DeleteFile((CONST_STRPTR)apath))
		return send_dos_error(id, IoErr());
	return send_status(id, SSH_FX_OK);
}

static int do_mkdir(ULONG id, struct req *r)
{
	char sftp[PATH_MAX_LEN], apath[PATH_MAX_LEN];
	const UBYTE *s;
	ULONG n;
	struct attrs a;
	int root;
	BPTR lock;

	if (!get_string(r, &s, &n) || !get_attrs(r, &a))
		return send_status(id, SSH_FX_BAD_MESSAGE);
	if (!request_path(s, n, sftp, apath, &root) || root)
		return send_status(id, SSH_FX_FAILURE);
	lock = CreateDir((CONST_STRPTR)apath);
	if (lock == 0)
		return send_dos_error(id, IoErr());
	UnLock(lock);
	if (a.flags & SSH_FILEXFER_ATTR_PERMISSIONS)
		SetProtection((CONST_STRPTR)apath, perm_to_prot(a.perm, 0));
	return send_status(id, SSH_FX_OK);
}

static int do_realpath(ULONG id, struct req *r)
{
	char sftp[PATH_MAX_LEN], apath[PATH_MAX_LEN];
	char ln[PATH_MAX_LEN + 80];
	const UBYTE *s;
	ULONG n;
	int root;

	if (!get_string(r, &s, &n))
		return send_status(id, SSH_FX_BAD_MESSAGE);
	if (!request_path(s, n, sftp, apath, &root))
		return send_status(id, SSH_FX_NO_SUCH_FILE);
	long_name(ln, sizeof(ln), sftp, S_IFDIR_BITS | 0755, 0, 0);
	ob_reset(&reply);
	ob_u8(&reply, SSH_FXP_NAME);
	ob_u32(&reply, id);
	ob_u32(&reply, 1);
	ob_cstring(&reply, sftp);
	ob_cstring(&reply, sftp);
	ob_u32(&reply, 0);		/* no attributes */
	(void)ln;
	return ob_send(&reply);
}

static int do_rename(ULONG id, struct req *r)
{
	char sftp1[PATH_MAX_LEN], apath1[PATH_MAX_LEN];
	char sftp2[PATH_MAX_LEN], apath2[PATH_MAX_LEN];
	const UBYTE *s1, *s2;
	ULONG n1, n2;
	int root1, root2;

	if (!get_string(r, &s1, &n1) || !get_string(r, &s2, &n2))
		return send_status(id, SSH_FX_BAD_MESSAGE);
	if (!request_path(s1, n1, sftp1, apath1, &root1)
	    || !request_path(s2, n2, sftp2, apath2, &root2))
		return send_status(id, SSH_FX_NO_SUCH_FILE);
	if (root1 || root2)
		return send_status(id, SSH_FX_PERMISSION_DENIED);
	/* Rename() refuses an existing target, as SFTP version 3 requires. */
	if (!Rename((CONST_STRPTR)apath1, (CONST_STRPTR)apath2))
		return send_dos_error(id, IoErr());
	return send_status(id, SSH_FX_OK);
}

/* ---- main loop ---------------------------------------------------------- */

/* Handles one request. Returns 0 when the server must end. */
static int dispatch(UBYTE type, struct req *r)
{
	ULONG id;

	if (type == SSH_FXP_INIT) {
		ob_reset(&reply);
		ob_u8(&reply, SSH_FXP_VERSION);
		ob_u32(&reply, SFTP_VERSION);	/* no extensions */
		return ob_send(&reply);
	}
	if (!get_u32(r, &id))
		return 1;	/* no id to answer: ignore the packet */
	switch (type) {
	case SSH_FXP_OPEN:     return do_open(id, r);
	case SSH_FXP_CLOSE:    return do_close(id, r);
	case SSH_FXP_READ:     return do_read(id, r);
	case SSH_FXP_WRITE:    return do_write(id, r);
	case SSH_FXP_LSTAT:
	case SSH_FXP_STAT:     return do_stat(id, r);
	case SSH_FXP_FSTAT:    return do_fstat(id, r);
	case SSH_FXP_SETSTAT:  return do_setstat(id, r);
	case SSH_FXP_FSETSTAT: return do_fsetstat(id, r);
	case SSH_FXP_OPENDIR:  return do_opendir(id, r);
	case SSH_FXP_READDIR:  return do_readdir(id, r);
	case SSH_FXP_REMOVE:   return do_remove(id, r, 0);
	case SSH_FXP_RMDIR:    return do_remove(id, r, 1);
	case SSH_FXP_MKDIR:    return do_mkdir(id, r);
	case SSH_FXP_REALPATH: return do_realpath(id, r);
	case SSH_FXP_RENAME:   return do_rename(id, r);
	default:
		/* READLINK, SYMLINK, EXTENDED and unknown types */
		return send_status(id, SSH_FX_OP_UNSUPPORTED);
	}
}

int main(void)
{
	struct Process *me = (struct Process *)FindTask(NULL);
	APTR oldwin = me->pr_WindowPtr;
	char apath[PATH_MAX_LEN];
	UBYTE lenb[4];
	UBYTE *pkt = NULL;
	ULONG cap = 0;
	int i;

	/* No requesters: nobody may be at the Amiga's screen. */
	me->pr_WindowPtr = (APTR)-1;
	in_fh = Input();
	out_fh = Output();

	/* The current directory, set by the console handler to the account's
	 * home directory, is where relative paths start. */
	if (NameFromLock(me->pr_CurrentDir, (STRPTR)apath, sizeof(apath)))
		amiga_to_sftp(apath, home_sftp, sizeof(home_sftp));
	else
		strcpy(home_sftp, "/");

	for (;;) {
		ULONG len;
		struct req r;

		if (!read_full(lenb, 4))
			break;		/* client closed the channel */
		len = ((ULONG)lenb[0] << 24) | ((ULONG)lenb[1] << 16)
			| ((ULONG)lenb[2] << 8) | lenb[3];
		if (len == 0 || len > MAX_PACKET)
			break;		/* not an SFTP client: stop */
		if (len > cap) {
			if (pkt != NULL)
				FreeVec(pkt);
			pkt = AllocVec(len, MEMF_ANY);
			cap = pkt ? len : 0;
			if (pkt == NULL)
				break;
		}
		if (!read_full(pkt, len))
			break;
		r.p = pkt + 1;
		r.len = len - 1;
		r.pos = 0;
		if (!dispatch(pkt[0], &r))
			break;		/* output gone */
	}

	for (i = 0; i < MAX_HANDLES; i++) {
		if (handles[i].type != H_FREE)
			close_handle(&handles[i]);
	}
	if (pkt != NULL)
		FreeVec(pkt);
	if (reply.d != NULL)
		FreeVec(reply.d);
	me->pr_WindowPtr = oldwin;
	return RETURN_OK;
}
