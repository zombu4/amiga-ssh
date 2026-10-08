# Building amiga-ssh from source

The prebuilt programs in `dist/` are what `build.sh` produces. This file
describes how to make them yourself: the sources and their exact versions,
the toolchain, every command, and what the build does.

## Sources

| What | Where | Version |
|---|---|---|
| Cross toolchain (gcc, binutils, libnix, NDK headers) | https://github.com/AmigaPorts/m68k-amigaos-gcc | commit `433b3f3839d3fcf79667d906bbca67a7a2b797b6` |
| libnix (the C library the port uses) | https://github.com/AmigaPorts/libnix, fetched by the toolchain into `projects/libnix` | commit `0a1d18a0938dce3f7a234963bcb7fa6148f6a6f3` |
| Dropbear SSH | https://github.com/mkj/dropbear | commit `17549ea6b6be0d59e99c5c5350185c0f9cc57211` (three commits after tag `DROPBEAR_2026.94`) |
| The port | this repository: `port/`, `patches/`, `build.sh` | |

`make update` in the toolchain fetches its projects. These are the commits of
each project the published programs were built with:

| Project (`projects/...`) | Repository | Commit |
|---|---|---|
| gcc | https://github.com/AmigaPorts/gcc | `26e100b31d25392a21068483d4c541a4b55822c3` |
| binutils | https://github.com/AmigaPorts/binutils-gdb | `486d13bafc19f15a65be861071bdee1381314f02` |
| libnix | https://github.com/AmigaPorts/libnix | `0a1d18a0938dce3f7a234963bcb7fa6148f6a6f3` |
| amiga-netinclude | https://github.com/AmigaPorts/amiga-netinclude | `7d155ca3a3090afe3466f819dd178f2af6f5c176` |
| newlib-cygwin | https://github.com/AmigaPorts/newlib-cygwin | `d8eff91f1916ee996c30f10e90984563cddc310f` |
| clib2 | https://github.com/AmigaPorts/clib2 | `842c0c66e6338e133b8ca1d57f85c8a7b346b115` |
| ixemul | https://github.com/AmigaPorts/ixemul | `f0dd997994263c6ac783b563222a9f53a00c013c` |
| libdebug | https://github.com/AmigaPorts/libdebug | `42d9d3a7a0d26fa4679384ffc78cf79fbce394d8` |
| sfdc | https://github.com/AmigaPorts/sfdc | `4e7afc044c1ff129574fcf3b839c3351edcfe2fb` |
| fd2sfd | https://github.com/AmigaPorts/fd2sfd | `189ac23b5b2d0d0555381404d75e0508c2e95920` |
| fd2pragma | https://github.com/AmigaPorts/fd2pragma | `bcffac5c89295baf1db033addc984312b4b4a722` |
| aros-stuff | https://github.com/AmigaPorts/aros-stuff | `86711c70569ffc6b70ee1fb99895866c1d9f30e4` |
| ira | https://github.com/AmigaPorts/ira | `bb7c11384a32c409341fdb0d16d715b7959a0290` |
| vasm | https://github.com/AmigaPorts/vasm | `de93f7dd067001532cff663730133601edb274cb` |
| vbcc | https://github.com/AmigaPorts/vbcc | `55a487d500dccccad806cf3916e90bcd95189ec9` |
| vlink | https://github.com/AmigaPorts/vlink | `ce46ccfda0e30eda5b12b9a5d0259d8eaefdefe9` |

Dropbear is never copied into this repository. `build.sh` clones it at the
pinned commit and applies the patches in `patches/`.

## 1. Host tools

### macOS

Install Homebrew (https://brew.sh/) and Xcode's command line tools, then the
packages the toolchain's README lists:

```
brew install autoconf automake bash bison coreutils curl flex gettext \
  gnu-sed gnu-tar grep make wget xz
```

Put the GNU versions first on `PATH` (also from the toolchain's README; add
these lines to your shell profile to keep them):

```
export PATH="$(brew --prefix bison)/bin:$(brew --prefix flex)/bin:$PATH"
for pkg in coreutils gnu-sed gnu-tar grep make; do
  export PATH="$(brew --prefix $pkg)/libexec/gnubin:$PATH"
done
```

### Linux

Debian, Ubuntu:

```
sudo apt install make curl gcc g++ flex bison gettext autoconf rsync libreadline-dev git
```

Fedora:

```
sudo dnf install curl gcc gcc-c++ patch autoconf automake make bison flex rsync ncurses-devel which git
```

These are the toolchain README's lists, plus `git` for `build.sh`.

`build.sh` itself needs only `git`, `make` and the standard shell tools
(`sh`, `awk`, `grep`, `getconf`, `id`).

## 2. The toolchain

```
git clone https://github.com/AmigaPorts/m68k-amigaos-gcc $HOME/amigaos/m68k-amigaos-gcc
cd $HOME/amigaos/m68k-amigaos-gcc
git checkout 433b3f3839d3fcf79667d906bbca67a7a2b797b6
make update
```

`make update` clones the projects (gcc, binutils, libnix, ...) into
`projects/`. To build exactly what the published programs were built with,
check out the commits from the table above in each project, for example:

```
git -C projects/libnix checkout 0a1d18a0938dce3f7a234963bcb7fa6148f6a6f3
git -C projects/gcc checkout 26e100b31d25392a21068483d4c541a4b55822c3
git -C projects/binutils checkout 486d13bafc19f15a65be861071bdee1381314f02
```

Then build and install the toolchain into `$HOME/amigaos/toolchain` (the
build installs as it goes; there is no separate install step):

```
make all PREFIX=$HOME/amigaos/toolchain
```

The compiler is then `$HOME/amigaos/toolchain/bin/m68k-amigaos-gcc`. The port
uses it with `-mcrt=nix20` (libnix for Kickstart 2.0 and later). The AmigaOS
NDK headers come with the toolchain
(`$HOME/amigaos/toolchain/m68k-amigaos/ndk-include`).

## 3. The port

```
cd amiga-ssh
./build.sh
```

To use a toolchain somewhere else, set `AMIGA_PREFIX` to its install prefix:

```
AMIGA_PREFIX=/opt/amiga ./build.sh
```

`build.sh` needs network access to clone Dropbear from GitHub.

### What build.sh does

1. **Checks** that the cross compiler, `nm` and `strip` exist under
   `$AMIGA_PREFIX/bin`, that `git` and `make` exist, and that every port
   source is present. It puts `$AMIGA_PREFIX/bin` first on `PATH`, so
   Dropbear's configure finds `m68k-amigaos-ar`, `-ranlib` and `-strip`.
2. **Fetches Dropbear**: deletes `build/`, clones
   https://github.com/mkj/dropbear.git into `build/dropbear`, checks out
   commit `17549ea6b6be0d59e99c5c5350185c0f9cc57211` and stops if `HEAD` is
   anything else.
3. **Applies the patches** in `patches/` in name order with `git apply`
   (which refuses a patch that does not apply cleanly), and copies
   `port/localoptions.h` into the Dropbear tree, where Dropbear's
   `options.h` includes it.
4. **Configures** Dropbear with `--host=m68k-amigaos`,
   `CC="m68k-amigaos-gcc -mcrt=nix20"`, `CPPFLAGS=-I<repo>/port`, and
   `--disable-zlib --disable-syslog --disable-lastlog --disable-utmp
   --disable-utmpx --disable-wtmp --disable-wtmpx --disable-loginfunc
   --disable-pututline --disable-pututxline --disable-harden
   --enable-static --enable-bundled-libtom`. Every one of these exists in
   Dropbear's `configure --help`; `build.sh` comments why each is used.
   The log goes to `build/configure.log`.
5. **Compiles the port's sources** (`port/*.c`) with the same flags as
   Dropbear's own files and Dropbear's include directories (after first
   letting Dropbear's Makefile generate `default_options_guard.h`).
   `port/amiga_stack.c` is compiled twice: with a 128 KB stack for `sshd`
   and `dropbearkey`, and a 32 KB stack for `sshpasswd` and `sshd-startup`.
6. **Builds `dropbear` and `dropbearkey`** with Dropbear's Makefile:
   `make PROGRAMS="dropbear dropbearkey"`, adding the port's objects through
   the `PORT_SVROBJS` and `PORT_KEYOBJS` variables (patch 0008), linking
   libnix's socket library (`-lsocket`) and writing a link map for each
   program. The log goes to `build/make.log`.
7. **Builds the helper programs** `sshd-console`, `sshpasswd` and
   `sshd-startup` (see the table below).
8. **Checks** the unstripped programs and stops on any failure:
   - `nm` must show none of `fork`, `vfork`, `execv`, `execve`, `execvp`,
     `execl`, `execlp`, `execle`, `pipe`, `link`, `symlink`;
   - the link maps must show none of libnix's `select.o`, `fcntl.o`,
     `getpwent.o`, `__inituser.o` or `system.o`.
9. **Installs** stripped copies into `dist/` as `sshd`, `dropbearkey`,
   `sshd-console`, `sshpasswd` and `sshd-startup`. The other files in
   `dist/` (`Install-SSH`, `Uninstall-SSH`, `shell-startup`, the licence
   files) are not touched.
10. **Checks `dist/`**: no file may contain the builder's home directory or
    user name as plain bytes. The compiler is run with
    `-ffile-prefix-map=<repo>=.` and `-fdebug-prefix-map=<repo>=.` so that
    the repository path is never recorded.

`build/` is scratch and listed in `.gitignore`. `dist/` is tracked: the
prebuilt programs ship in the repository.

### The programs and what each links

| Program | Built from |
|---|---|
| `sshd` | Dropbear's server (`dropbear`, patched) + `port/amiga_main.c`, `amiga_net.c`, `amiga_user.c`, `amiga_random.c`, `amiga_crypt.c`, `amiga_session.c`, `amiga_log.c`, `amiga_stack.c` (128 KB) + bundled libtomcrypt and libtommath + libnix with `-lsocket` |
| `dropbearkey` | Dropbear's key generator (patched) + `port/amiga_user.c`, `amiga_random.c`, `amiga_net.c`, `amiga_stack.c` (128 KB) + libtomcrypt, libtommath + libnix with `-lsocket` |
| `sshd-console` | `port/amiga_console.c` alone, + libnix (it calls bsdsocket.library itself). `sshd` loads it at run time from `PROGDIR:sshd-console`, then `C:sshd-console`, and gives it its stack when it starts it. |
| `sshpasswd` | `port/amiga_passwd.c`, `amiga_rewrite.c`, `amiga_crypt.c`, `amiga_random.c`, `amiga_stack.c` (32 KB) + libtomcrypt's `src/hashes/sha2/sha256.o` and `src/misc/crypt/crypt_argchk.o` from the Dropbear build + libnix |
| `sshd-startup` | `port/amiga_startup.c`, `amiga_rewrite.c`, `amiga_stack.c` (32 KB) + libnix |

## libnix defects the port works around

All paths are in libnix's source tree (`projects/libnix/sources` of the
toolchain) at commit `0a1d18a`.

| Defect | Where | What the port does |
|---|---|---|
| `select()` creates a timer request (line 27) but never opens `timer.device`, and `SendIO()`s it (line 165) for any call with a timeout. It also passes the highest socket number, not that number plus one, as the count to `WaitSelect()` (`socket/socket/__initsock.c:161-162`, `socket/unistd/select.c:187`). | `socket/unistd/select.c` | `port/amiga_net.c` defines its own `select()` and `amiga_waitselect()` over `WaitSelect()`. |
| `fcntl()` returns -1 for every call and sets no `errno`. | `nix/extra/fcntl.c:4-6` | `port/amiga_net.c` defines `fcntl()` for `F_GETFL`/`F_SETFL`, mapping `O_NONBLOCK` to `FIONBIO`. |
| Closing a socket descriptor closes the socket but never frees the descriptor slot (`_sock_close` does not clear `lx_inuse`). | `socket/socket/__initsock.c:41-61` | `port/amiga_net.c` creates every socket descriptor itself (`socket()`, `accept()`, `socketpair()`) with a close routine that frees the slot. |
| `fork()` always returns 0, which tells the caller it is the child; `execv()` runs the command synchronously with `System()`. | `nix20/extra/system.c:27-53` | Patch 0006 compiles out every `fork()`/`exec`/`pipe()` user in the server; `build.sh` fails if any of these symbols or `system.o` is linked. The listener (`port/amiga_main.c`) starts a new copy of the program per connection, and `port/amiga_session.c` starts shells and commands. |
| `getpwnam()` and `getgroups()` call `usergroup.library` whenever a bsdsocket.library stack is running; that library is absent on most systems. | `socket/unistd/getpwent.c:289-303`, `socket/unistd/__inituser.c:100-104` | `port/amiga_user.c` defines the user functions for the one configured account; `build.sh` fails if `getpwent.o` or `__inituser.o` is linked. |
| `getuid()` and its relatives always return 0 (root). | `nix/extra/getppid.c:3-9` | `port/amiga_user.c` defines them with the account's id, 100. |
| `signal()` accepts only signal numbers 1 to 6 and returns `SIG_ERR` for any other; there is no `sigaction()`. With the toolchain's headers `SIGTERM` is 15, `SIGPIPE` 13 and `SIGSEGV` 11. | `nix/signal/signal.c:9-10` | Patch 0003 sets only the `SIGINT` handler (libnix raises `SIGINT` for Ctrl-C, `nix/signal/__chkabort.c`); patch 0002 installs no `SIGCHLD` handler. |
| `<time.h>` declares `nanosleep()` only if `_POSIX_TIMERS` is defined, which it is not, although libnix has the function. | `sys-include/time.h:216` of the toolchain, `nix/extra/nanosleep.c` | Patch 0007 declares it in `svr-auth.c`. |
| `link()` is `symlink()`, a soft link; `open()` ignores `O_EXCL`. Seen on AmigaOS 3.2.2: dropbearkey's key file became a soft link to its deleted temporary file. | `nix20/misc/symlink.c`, `nix/stdio/open.c:33-38` | Patch 0009 writes host keys straight to their name after `stat()` shows nothing is there; `build.sh` fails if `link` or `symlink` is linked. |
| `dup2()` copies the descriptor structure, so both descriptors share one socket and closing either closes it. | `nix/stdio/dup2.c:51-53` | The session's connection is not moved to descriptor 0 with `dup2()`; patch 0003 makes `main_inetd()` use the descriptor the port took the connection into. |

## Making host keys on another computer

`Install-SSH` generates the host keys on the Amiga from the random pool it
has gathered at that moment (docs/DESIGN.md, Randomness). To use keys made
from a stronger random source instead, generate them on another computer with
Dropbear's own `dropbearkey`, built from the same Dropbear source:

```
git clone https://github.com/mkj/dropbear.git
cd dropbear
git checkout 17549ea6b6be0d59e99c5c5350185c0f9cc57211
./configure
make PROGRAMS=dropbearkey
./dropbearkey -t ed25519 -f dropbear_ed25519_host_key
./dropbearkey -t ecdsa -f dropbear_ecdsa_host_key
./dropbearkey -t rsa -f dropbear_rsa_host_key
```

Dropbear's key files have the same format on every platform. Copy the three
files unchanged (binary transfer) into `DEVS:Internet/sshd/` on the Amiga,
with exactly these names, replacing the ones the installer made. `sshd`
reads them when each connection starts, so no restart is needed. The files
are private keys: delete them from the other computer once they are copied,
and keep `DEVS:Internet/sshd/` away from other people.
