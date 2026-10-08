# amiga-ssh design

How the Dropbear SSH server is made to run on AmigaOS 3.x (68k). Every
statement about upstream code names the file and line in Dropbear at commit
`17549ea6b6be0d59e99c5c5350185c0f9cc57211` (three commits after tag
`DROPBEAR_2026.94`). Statements about the C runtime name the file in libnix as
shipped with the AmigaPorts m68k-amigaos-gcc toolchain (libnix commit
`0a1d18a`).

## Goals

- An SSH server for a stock AmigaOS 3.x machine with any TCP/IP stack that
  provides `bsdsocket.library` (AmiTCP API, version 4 or later).
- Login with a password or a public key. The installer sets the password
  `amiga`; the `sshpasswd` command changes it.
- Interactive shells (with or without a terminal) and single commands
  (`ssh amiga list`), with the command's real return code sent back to the
  client.
- Several sessions at once.
- Runs entirely in fast RAM where the machine has it; never asks for chip RAM.
- Upstream Dropbear is fetched at a pinned commit and patched at build time.
  It is never copied into this repository.

## What AmigaOS lacks, and what replaces it

| Upstream needs | Where | Replacement |
|---|---|---|
| `fork()` per connection | svr-main.c:313 | a listener that starts a fresh copy of the server for each connection (see Process model) |
| `fork()`/`execv()` for the shell | dbutil.c:274-366, svr-chansession.c:813-938 | a console handler process plus `SystemTags()` (see Sessions) |
| `pipe()` | common-session.c:95, dbutil.c:291-299 | a connected pair of 127.0.0.1 TCP sockets |
| `select()` | common-session.c:211, dbutil.c:826 | our `select()` over `WaitSelect()` (see Waiting) |
| `SIGCHLD` + `waitpid()` | svr-chansession.c:95-164 | an exec message from the console handler that carries the return code |
| `/dev/urandom` | dbrandom.c:259 (fatal if missing) | an entropy pool kept in a seed file (see Randomness) |
| `getpwnam()`, `setuid()` | common-session.c:667, svr-auth.c:501-545 | one fixed account, `DROPBEAR_SVR_MULTIUSER 0` |
| `crypt()` | svr-authpasswd.c:71 | SHA-256 crypt (`$5$`), the standard scheme, in the port |
| pty, termios, utmp | sshpty.c, svr-chansession.c:591-638 | handled inside the console handler; utmp compiled out |

### libnix routines the port replaces or avoids

Each was read in full before the decision; the first crashed a test machine.
BUILDING.md lists them with file and line.

- `select()` (`sources/socket/unistd/select.c`) creates a timer request at
  line 27 but never opens `timer.device`, then `SendIO()`s that request at
  line 165 for any call with a timeout. It also passes the highest socket
  number, not that number plus one, as the count to `WaitSelect()`.
  Replaced in port/amiga_net.c.
- `fcntl()` returns -1 for every call without setting `errno`; closing a
  socket descriptor never frees its slot. Both replaced in port/amiga_net.c.
- `fork()` (`sources/nix20/extra/system.c`) always returns 0, which tells the
  caller it is the child. Patch 0006 compiles out every user of it.
- `getpwnam()`/`getgroups()` call usergroup.library whenever a TCP/IP stack
  runs, and `getuid()` returns 0. Replaced in port/amiga_user.c.
- `link()` is `symlink()` (`sources/nix20/misc/symlink.c`), and `open()`
  ignores `O_EXCL`. Patch 0009 makes dropbearkey write keys without them;
  found on real hardware, where the key file became a dangling soft link.

The build fails if any program contains `fork`, `vfork`, the `exec` family,
`pipe`, `link` or `symlink`, or links libnix's `select`, `fcntl`,
`getpwent`, `__inituser` or `system` objects.

## Process model

`sshd` has two modes.

**Listener** (default). Waits until `bsdsocket.library` can be opened (so it
can be started from `S:User-Startup` before the network is up), opens TCP
port 22 (or every `-p`), waits with `WaitSelect()`, and for each accepted
connection:

1. stirs the connection's arrival time and peer address into the random
   pool and writes fresh output to the seed file;
2. makes a loopback socket pair for the session's login state (below) and
   hands the connection and one end of that pair to the network stack with
   `ReleaseCopyOfSocket(sock, UNIQUE_ID)`, which returns an id for each;
3. starts `sshd -i -S <sockid>,<pipeid> <options>` with `SystemTags()` and
   `SYS_Asynch`, with `NIL:` as input and output. AmigaOS loads a separate
   copy of the program, so the new process has its own global data, which
   Dropbear's single-session design requires;
4. closes its own copies and goes back to waiting.

At most `MAX_UNAUTH_CLIENTS` (4) connections, and `MAX_UNAUTH_PER_IP` (2)
from one address, may be waiting for login at once (port/localoptions.h;
upstream's defaults of 30 and 5 assume cheap forked processes). The session
closes its end of the pair after a successful login (svr-auth.c:475) or
when it ends, which frees the slot; a slot whose session never started is
freed after the login timeout. This needs only `bsdsocket.library` calls, so
it is not tied to one stack.

**Session** (`-i`). Takes both sockets with `ObtainSocket(id, AF_INET,
SOCK_STREAM, 0)` and runs upstream's `main_inetd()` (svr-main.c:97-127) on
the connection's descriptor; patch 0003 makes it use that descriptor
instead of descriptor 0. The process serves one connection and exits
through `dropbear_close()`/`dropbear_exit()` (svr-session.c:220-316).

Roadshow users may instead let the stack's own superserver start `sshd -i` for
each connection, with one line in `DEVS:Internet/servers`. libnix already
picks up a socket handed over that way (`__initsock.c`, `init_inet_daemon`).
This is optional; the listener mode is the default because it works on every
stack.

## Waiting

Every descriptor the session waits on is a socket: the client connection,
the loopback pairs that stand in for pipes, and forwarded ports. Our
`select()` replaces libnix's. For each requested descriptor it uses libnix's
public descriptor table (`_lx_fhfromfd()`, `LX_SOCKET`, `lx_sock`, libnix
`stdio.h:243-289`) to find the real socket number, builds `fd_set`s of real
socket numbers, calls `WaitSelect(max + 1, ..., timeout, &signals)` and maps
the result back. A descriptor that is not a socket is refused with `EBADF`,
so a wrong descriptor fails loudly instead of being ignored.

`WaitSelect()` also waits on Amiga signals. The session uses one signal bit
for "a console handler has finished", which is how child exit reaches the
main loop without `SIGCHLD`.

## Sessions

For each `shell` or `exec` request the session process:

1. Creates a loopback socket pair. One end becomes the channel's
   `readfd`/`writefd`, exactly as the pipe ends do upstream
   (dbutil.c:355-363). The session hands the other end to the console handler
   with `ReleaseCopyOfSocket()`/`ObtainSocket()`, because a socket belongs to
   the library base of the task that opened it.
2. Starts the console handler as a new process. The handler is a minimal
   AmigaDOS handler: it answers the DOS packets an interactive shell sends to
   its console (`FINDINPUT`/`FINDOUTPUT`, `READ`, `WRITE`, `WAIT_CHAR`,
   `SCREEN_MODE`, `CHANGE_SIGNAL`, `DISK_INFO`, `IS_FILESYSTEM`, `SEEK`,
   `END`) and moves the bytes to and from its socket.
3. A helper process started by the handler calls `SystemTags()`, with
   `"NewShell *"` for a shell or the command text for `exec`, with its input
   and output on the handler, and blocks there. When it returns, the helper
   has the shell's or the command's return code.
4. The handler sends the session an exec message with that code and signals
   it. The session's main loop sees the signal through `WaitSelect()`, fills
   `chansess->exit` the way `svr_chansess_checksignal()` does
   (svr-chansession.c:95-141) and sets `ses.channel_signal_pending`, after
   which upstream's own close sequence sends `exit-status` and closes the
   channel (common-channel.c:276-393).

When the client requests a terminal (`pty-req`), the handler edits lines
itself in the console's cooked mode (echo, Backspace/Del, Ctrl-U) and passes
bytes straight through once the shell switches the console to raw mode; it
turns Ctrl-C/D/E/F into break signals for the shell and LF into CR LF on
output, as the Amiga console does. Without a terminal it passes bytes
through unchanged. The upstream pty path (`pty_allocate`, `tcsetattr`,
`TIOCSWINSZ`, utmp) is replaced, not emulated. The window size from the
client is stored in the control block; the handler does not yet answer a
program's window-size query with it.

`NewShell` runs the new shell asynchronously (dos.doc, SystemTagList,
lines 5884-5888), so for an interactive shell `SystemTags()` returns as soon
as the shell has started. The handler therefore ends a shell session when
every console handle has been closed, and the exit status sent to the
client is `NewShell`'s own return code. For `exec` the command runs
synchronously and the client gets the command's real return code.

The process-handling design follows the approach used by telnetd for AmigaOS (github.com/toralux/amiga-telnetd, MIT, by Tor Anders
Johansen). That design calls `SystemTags()` from a helper that blocks until
the shell exits, never from the daemon. No code is taken from it.

## File transfer (SFTP)

SFTP is an SSH subsystem: for a channel that asks for `sftp`, Dropbear runs
`SFTPSERVER_PATH` (svr-chansession.c:696-704) as an `exec` command, which
goes through the console handler like any other command. In its
non-terminal mode the handler passes bytes unchanged; measured on the
reference machine, all 256 byte values and 1 MB of random data came back
byte-identical through a program that copies its input to its output with
`Read()`/`Write()`.

`sftp-server` (port/amiga_sftp.c) is the port's own SFTP server, written
for AmigaOS with dos.library calls only. It speaks protocol version 3
(draft-ietf-secsh-filexfer-02), which is what OpenSSH's `sftp` client and
its `scp` (OpenSSH 9.0 and later use SFTP for `scp`) speak. It reads
requests from `Input()` and answers on `Output()`, one request at a time.

Paths. SFTP clients expect one tree under `/`; AmigaOS has volumes and
assigns. The mapping is purely textual:

| SFTP path | AmigaOS |
|---|---|
| `/` | a virtual directory listing the mounted volumes and the assigns |
| `/Work` | `Work:` |
| `/Work/dir/file` | `Work:dir/file` |
| relative paths | relative to the account's `home=` directory |

`.` and `..` are resolved in the text before the mapping, so `..` never
leaves `/`. A component that contains `:` is refused. `REALPATH` returns
the resolved SFTP path; `REALPATH .` returns the home directory, which is
where clients start.

Attributes. Size comes from the FileInfoBlock. Permissions are built from
the protection bits: the owner bits from the inverted `FIBF_READ`,
`FIBF_WRITE` and `FIBF_EXECUTE`, the group and other bits from the
`FIBF_GRP_*` and `FIBF_OTR_*` bits, plus the file-type bits for files and
directories. The modification time is the DateStamp converted to seconds
since 1970 (`(days + 2922) * 86400 + minutes * 60 + ticks / 50`; 2922 days
lie between 1970-01-01 and 1978-01-01). AmigaOS keeps local time, so times
are shown as if the Amiga clock ran on UTC. `SETSTAT` changes the
protection bits (`SetProtection()`), the date (`SetFileDate()`) and the
size (`SetFileSize()`); owner and group do not exist and are ignored.

Requests. `OPEN` maps the flags to `MODE_OLDFILE` (read, or write without
create), `MODE_READWRITE` (create without truncate) or `MODE_NEWFILE`
(truncate); `EXCL` first checks that the name is free. `READ` and `WRITE`
seek only when the offset differs from the current position. `OPENDIR` and
`READDIR` use `Examine()`/`ExNext()` and send up to 64 names per reply.
`REMOVE` and `RMDIR` use `DeleteFile()`, `MKDIR` `CreateDir()`, `RENAME`
`Rename()` (which, like SFTP version 3, refuses an existing target).
`READLINK` and `SYMLINK` answer `OP_UNSUPPORTED`; soft links
(`ST_SOFTLINK`, which has a positive entry type like a directory) are
listed as symbolic links, not as directories. DOS errors map to
`NO_SUCH_FILE` (object or directory not found), `PERMISSION_DENIED`
(protected or write-protected) or `FAILURE` with the DOS error text.
Requesters are off (`pr_WindowPtr = -1`), so a missing volume is an error,
never a wait.

Throughput. The first measurement took 247 s to download 1 MB. The cause
was in the console handler: a WRITE larger than its output buffer (then
4 KB) was taken one buffer at a time, and after each piece the handler
slept until its next wake-up, up to a second. It now keeps taking and
sending until the WRITE is done or the socket is full, does not sleep
while WRITE data waits, and has a 16 KB buffer. Measured afterwards on the
reference machine: 1 MB up in 6.8 s and down in 5.2 s, each including
about 2.8 s of login.

## Randomness

Upstream refuses to start without its random device (dbrandom.c:259-262);
the path is a compile-time setting (sysoptions.h:379-381). The port replaces
that read with an Amiga entropy pool:

- a seed file, read at start and rewritten with fresh output before the
  session starts, so every boot and connection adds to the pool gathered
  before;
- timing jitter measured with `ReadEClock()` across many short waits;
- the arrival time of each connection, measured with `ReadEClock()` by the
  listener, which stirs it and the peer address into the seed file before
  the session starts;
- `DateStamp()`, `AvailMem()`, the task address and the socket's peer address.

Everything is hashed into the pool with SHA-256, which upstream already does
for its own inputs (dbrandom.c:223-310). The installation is hands-off: it
asks for no typing or mouse movement and generates the host keys with
whatever the pool holds at that moment. How much entropy each source gives on
a machine depends on its hardware. Measured on the reference machine (A2000
with PiStorm, AmigaOS 3.2.2; EClock 709379 Hz): 64 one-microsecond waits took
between 119 and 1724 EClock ticks, with 38 distinct values; the fixed
computation took 14 ticks in almost every run (5 distinct values in 64), so
it adds little. Anyone who wants host keys from a stronger
source can generate them on another computer with `dropbearkey` and copy them
into `DEVS:Internet/sshd/` (see BUILDING.md).

## Accounts and files

`DROPBEAR_SVR_MULTIUSER 0`. Upstream then requires `getgroups()` to fail with
`ENOSYS` (common-session.c:71-82), which is accurate on AmigaOS. A fixed
`getpwnam()` returns the one account the installer configured. Upstream looks
for `authorized_keys` in `svr_opts.authorized_keys_dir`, default `~/.ssh`
(svr-runopts.c:186, svr-authpubkey.c:463-478); the port sets that directory
to `DEVS:Internet/sshd`. `setuid()` and the ownership checks
on `authorized_keys` and its directories (svr-authpubkey.c:578-642) have
no owners to check on AmigaOS. They are replaced with a check that the file
exists and can be read.

The installer sets up the login `amiga` with the password `amiga` and asks
no questions. The name can be changed later in the config file's `user=`
line, the password with `sshpasswd`. Logins under any other name are
refused, as upstream does for unknown users (svr-auth.c:263-270).

The password is stored only as a SHA-256 crypt hash (`$5$<salt>$<hash>`,
the scheme glibc and OpenSSL call SHA-crypt), in the config file's
`password=` line. Dropbear checks a login with
`crypt(password, stored hash)` and a constant-time comparison
(svr-authpasswd.c:70-71); the port supplies that `crypt()`. Because the
scheme is the standard one, a hash made with `openssl passwd -5` on any
other computer is accepted, and the port's output can be checked against
it. `sshpasswd` asks for the new password twice, without echo, and rewrites
that line. `amiga` is a well-known default: anyone who can reach port 22
can log in with it until it is changed, and the installer says so.

All server files live in `DEVS:Internet/sshd/`, next to the TCP/IP
configuration in `DEVS:Internet/`:

| File | Purpose |
|---|---|
| `config` | the login name, the home directory and the password hash |
| `dropbear_ed25519_host_key`, `dropbear_ecdsa_host_key`, `dropbear_rsa_host_key` | host keys |
| `authorized_keys` | public keys allowed to log in |
| `random_seed` | the saved entropy pool |
| `log`, `log.old` | the log of the listener and every session (patch 0010, port/amiga_log.c): each line Dropbear prints to stderr is also appended here, with a shared lock per line; at 16 KB `log` is renamed to `log.old` |
| `shell-startup` | start-up script of SSH shells (NEWSHELL's FROM): runs `S:Shell-Startup`, then adds the aliases `exit` and `logout` for `EndShell` |

## Build settings

`localoptions.h` turns off, against `default_options.h`:

- multi-user support and dropping privileges (lines 264, 333)
- agent forwarding (line 80)
- stream forwarding, which needs Unix sockets (lines 76-77)
- re-exec (line 49), which is Linux-only anyway (sysoptions.h:45-49)
- the message of the day (line 237)
- the SFTP subsystem (line 358), because there is no `sftp-server` for
  AmigaOS

Cryptography stays at upstream's defaults: Ed25519, ECDSA and RSA-SHA256 host
keys; curve25519, sntrup761 and ML-KEM key exchange; SHA-256 MACs. SHA-1 and
DSS stay off.

## Memory

The linker emits code and data blocks without a chip-RAM flag, and libnix's
`malloc()` asks for `MEMF_PUBLIC`/`MEMF_ANY` (`sources/nix/stdlib/malloc.c`
lines 80 and 169). On a machine with fast RAM the program and its data
therefore come from fast RAM. The installer test records `Avail CHIP` before
and after starting the server to confirm this on real hardware.
