#!/bin/sh
#
# build.sh - builds the AmigaOS port of the Dropbear SSH server.
#
# Copyright (c) 2026 zombu2
# SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
#
# Run on macOS or Linux with the m68k-amigaos-gcc toolchain installed
# (see BUILDING.md). One command, no arguments:
#
#   ./build.sh
#
# AMIGA_PREFIX selects the toolchain (default: $HOME/amigaos/toolchain).
#
# The script fetches Dropbear at the pinned commit into build/, applies
# patches/, builds the server and its helper programs with the port's
# sources in port/, checks the results and puts the stripped programs in
# dist/:
#
#   sshd          the server (Dropbear's "dropbear" plus the port)
#   dropbearkey   Dropbear's host key generator
#   sshd-console  the console handler the server starts for each shell
#   sshpasswd     changes the login password
#   sshd-startup  edits S:User-Startup for Install-SSH and Uninstall-SSH
#   sftp-server   the SFTP file transfer server
#
# Everything else in dist/ (Install-SSH, Uninstall-SSH, their .info icons,
# the licence files) is left as it is. Any failed step or check stops the
# script with an error.

set -eu

# ---- settings -------------------------------------------------------------

# The repository is the directory this script is in.
REPO=$(cd "$(dirname "$0")" && pwd)

# Toolchain prefix; its bin/ directory holds m68k-amigaos-gcc.
AMIGA_PREFIX=${AMIGA_PREFIX:-$HOME/amigaos/toolchain}

# Dropbear upstream and the commit the patches are made for (three commits
# after the tag DROPBEAR_2026.94).
DROPBEAR_URL=https://github.com/mkj/dropbear.git
DROPBEAR_COMMIT=17549ea6b6be0d59e99c5c5350185c0f9cc57211

# Work directories: build/ is scratch (in .gitignore), dist/ is shipped.
BUILD=$REPO/build
SRC=$BUILD/dropbear
OBJ=$BUILD/port
DIST=$REPO/dist
PORT=$REPO/port

# The cross tools.
TARGET=m68k-amigaos
CC_BIN=$AMIGA_PREFIX/bin/$TARGET-gcc
NM=$AMIGA_PREFIX/bin/$TARGET-nm
STRIP=$AMIGA_PREFIX/bin/$TARGET-strip

# configure looks for the target's ar, ranlib and strip by name
# (m68k-amigaos-ar, ...); without the toolchain on PATH it would take the
# host's ar and make archives the cross linker cannot read.
PATH=$AMIGA_PREFIX/bin:$PATH
export PATH

# libnix as the C library (-mcrt=nix20: libnix for Kickstart 2.0 and
# later).
CC="$CC_BIN -mcrt=nix20"

# Compiler flags for every C file of this build. -ffile-prefix-map and
# -fdebug-prefix-map replace the repository path with "." wherever the
# compiler would record it (__FILE__, debug information), so no local path
# ends up in a program. -Os -W -Wall is what Dropbear's configure uses when
# CFLAGS is not set; giving CFLAGS replaces that default, so it is repeated
# here.
CFLAGS_COMMON="-Os -W -Wall -ffile-prefix-map=$REPO=. -fdebug-prefix-map=$REPO=."

# Number of parallel make jobs.
JOBS=$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 2)

# Prints a step heading.
step() {
	echo "==> $*"
}

# Stops the script with a message.
fail() {
	echo "build.sh: $*" >&2
	exit 1
}

# ---- 1. check the tools ----------------------------------------------------

step "Checking the toolchain in $AMIGA_PREFIX"
for tool in "$CC_BIN" "$NM" "$STRIP"; do
	[ -x "$tool" ] || fail "missing $tool (set AMIGA_PREFIX)"
done
command -v git >/dev/null 2>&1 || fail "git is not installed"
command -v make >/dev/null 2>&1 || fail "make is not installed"

# Every port source the build needs must be present.
for f in amiga_main.c amiga_net.c amiga_user.c amiga_random.c \
		amiga_crypt.c amiga_session.c amiga_console.c amiga_passwd.c \
		amiga_rewrite.c amiga_startup.c amiga_stack.c amiga_log.c amiga_sftp.c \
		localoptions.h; do
	[ -f "$PORT/$f" ] || fail "missing port/$f"
done

# ---- 2. fetch Dropbear at the pinned commit ------------------------------------

# A fresh clone every time, so no earlier build or edit can leak in.
step "Fetching Dropbear $DROPBEAR_COMMIT"
rm -rf "$BUILD"
mkdir -p "$BUILD"
git clone --quiet "$DROPBEAR_URL" "$SRC"
git -C "$SRC" -c advice.detachedHead=false checkout --quiet "$DROPBEAR_COMMIT"

# The checked-out commit must be exactly the pinned one.
HEAD=$(git -C "$SRC" rev-parse HEAD)
[ "$HEAD" = "$DROPBEAR_COMMIT" ] || fail "checked out $HEAD, expected $DROPBEAR_COMMIT"

# ---- 3. apply the port's patches -------------------------------------------------

# In name order; git apply refuses a patch that does not apply cleanly.
step "Applying patches"
for p in "$REPO"/patches/*.patch; do
	echo "    $(basename "$p")"
	git -C "$SRC" apply "$p"
done

# Dropbear's options.h includes localoptions.h from the build directory.
cp "$PORT/localoptions.h" "$SRC/localoptions.h"

# ---- 4. configure ------------------------------------------------------------------

# --host: cross-compile for AmigaOS. Each feature below is left out because
# AmigaOS or libnix has no use for it:
#   --disable-zlib       no zlib for libnix; compression is not built
#   --disable-syslog     AmigaOS has no syslog(); messages go to stderr
#   --disable-lastlog, --disable-utmp, --disable-utmpx, --disable-wtmp,
#   --disable-wtmpx, --disable-loginfunc, --disable-pututline,
#   --disable-pututxline
#                        AmigaOS keeps no login records
#   --disable-harden     the hardening flags (-fstack-protector, PIE,
#                        RELRO, ...) are for ELF platforms
#   --enable-static      one self-contained program
#   --enable-bundled-libtom
#                        use the libtomcrypt and libtommath in Dropbear's
#                        tree (there are no system copies for AmigaOS)
# CPPFLAGS adds port/ to the include path for the port's headers, which
# localoptions.h and the patches include.
step "Configuring"
(
	cd "$SRC"
	./configure --host=$TARGET \
		CC="$CC" \
		CFLAGS="$CFLAGS_COMMON" \
		CPPFLAGS="-I$PORT" \
		--disable-zlib \
		--disable-syslog \
		--disable-lastlog \
		--disable-utmp \
		--disable-utmpx \
		--disable-wtmp \
		--disable-wtmpx \
		--disable-loginfunc \
		--disable-pututline \
		--disable-pututxline \
		--disable-harden \
		--enable-static \
		--enable-bundled-libtom \
		> "$BUILD/configure.log" 2>&1
) || fail "configure failed, see build/configure.log"

# ---- 5. compile the port's sources -----------------------------------------------

# Same flags as Dropbear's own files (configure adds -Wundef,
# -fno-strict-overflow and -Wno-pointer-sign; the Makefile adds
# -DLOCALOPTIONS_H_EXISTS and, when the server is built, -DDROPBEAR_SERVER),
# and Dropbear's include directories: amiga_main.c uses Dropbear's
# headers, amiga_crypt.c and amiga_passwd.c use libtomcrypt's.
step "Compiling the port"
# Dropbear's options.h includes default_options_guard.h, which its Makefile
# generates from default_options.h; make it before the port uses options.h.
make -C "$SRC" default_options_guard.h > "$BUILD/guard.log" 2>&1 \
	|| fail "make default_options_guard.h failed, see build/guard.log"
mkdir -p "$OBJ"
PORT_CFLAGS="$CFLAGS_COMMON -Wundef -fno-strict-overflow -Wno-pointer-sign"
PORT_CPPFLAGS="-DLOCALOPTIONS_H_EXISTS -DDROPBEAR_SERVER -I$PORT -I$SRC -I$SRC/src -I$SRC/libtomcrypt/src/headers -I$SRC/libtommath"
for f in amiga_main amiga_net amiga_user amiga_random amiga_crypt \
		amiga_session amiga_console amiga_passwd amiga_rewrite \
		amiga_startup amiga_log amiga_sftp; do
	echo "    $f.c"
	$CC $PORT_CFLAGS $PORT_CPPFLAGS -c "$PORT/$f.c" -o "$OBJ/$f.o"
done

# The stack size (port/amiga_stack.c): 128 KB for sshd and dropbearkey
# (the default in the file), 32 KB for sshpasswd and sshd-startup, whose
# own stack frames are at most about 1.2 KB each (gcc -fstack-usage).
echo "    amiga_stack.c (128 KB and 32 KB)"
$CC $PORT_CFLAGS -c "$PORT/amiga_stack.c" -o "$OBJ/amiga_stack.o"
$CC $PORT_CFLAGS -DAMIGA_STACK_SIZE=32768 -c "$PORT/amiga_stack.c" \
	-o "$OBJ/amiga_stack_small.o"

# ---- 6. build Dropbear with the port ------------------------------------------------

# PORT_SVROBJS and PORT_KEYOBJS are the hooks the Makefile patch adds.
#   sshd:        main and listener, socket layer, account, entropy,
#                crypt(), sessions, log file, stack size
#   dropbearkey: account (getpwuid for "~" paths), entropy (seedrandom),
#                socket layer (select() for dbutil.c), stack size
# LIBS: libnix's socket library (bsdsocket.library calls behind the
# descriptors) and a link map per program, used by the checks below.
# Objects come before libraries on the link line, so the port's select(),
# fcntl(), socket(), accept(), getpwnam() and friends replace libnix's.
step "Building sshd and dropbearkey"
SVROBJS="$OBJ/amiga_main.o $OBJ/amiga_net.o $OBJ/amiga_user.o $OBJ/amiga_random.o $OBJ/amiga_crypt.o $OBJ/amiga_session.o $OBJ/amiga_log.o $OBJ/amiga_stack.o"
KEYOBJS="$OBJ/amiga_user.o $OBJ/amiga_random.o $OBJ/amiga_net.o $OBJ/amiga_stack.o"
make -C "$SRC" -j"$JOBS" PROGRAMS="dropbear dropbearkey" \
	PORT_SVROBJS="$SVROBJS" PORT_KEYOBJS="$KEYOBJS" \
	LIBS='-lsocket -Wl,-Map,$@.map' \
	> "$BUILD/make.log" 2>&1 || fail "make failed, see build/make.log"

# ---- 7. build the helper programs -------------------------------------------------

# sshd-console: the console handler; it calls bsdsocket.library itself.
# sshd starts it with CreateNewProc() and gives it its stack there
# (port/amiga_session.c), so amiga_stack.o is not linked.
step "Building sshd-console"
$CC -o "$BUILD/sshd-console" "$OBJ/amiga_console.o" \
	-Wl,-Map,"$BUILD/sshd-console.map"

# sshpasswd: SHA-256 crypt of the new password, a salt from the entropy
# samples, and the config file rewrite. SHA-256 comes from the libtomcrypt
# objects of the Dropbear build; sha256.o needs crypt_argchk.o for its
# argument checks.
step "Building sshpasswd"
LTC=$SRC/libtomcrypt/src
for o in "$LTC/hashes/sha2/sha256.o" "$LTC/misc/crypt/crypt_argchk.o"; do
	[ -f "$o" ] || fail "missing $o from the Dropbear build"
done
$CC -o "$BUILD/sshpasswd" "$OBJ/amiga_passwd.o" "$OBJ/amiga_rewrite.o" \
	"$OBJ/amiga_crypt.o" "$OBJ/amiga_random.o" "$OBJ/amiga_stack_small.o" \
	"$LTC/hashes/sha2/sha256.o" "$LTC/misc/crypt/crypt_argchk.o" \
	-Wl,-Map,"$BUILD/sshpasswd.map"

# sftp-server: the SFTP server sshd runs for file transfer; dos.library
# only, so it needs nothing but its own object and the stack size.
step "Building sftp-server"
$CC -o "$BUILD/sftp-server" "$OBJ/amiga_sftp.o" "$OBJ/amiga_stack_small.o" \
	-Wl,-Map,"$BUILD/sftp-server.map"

# sshd-startup: edits S:User-Startup for the installer.
step "Building sshd-startup"
$CC -o "$BUILD/sshd-startup" "$OBJ/amiga_startup.o" "$OBJ/amiga_rewrite.o" \
	"$OBJ/amiga_stack_small.o" \
	-Wl,-Map,"$BUILD/sshd-startup.map"

# Unstripped programs and their link maps, for the checks: name, program,
# map.
PROGRAMS="sshd:$SRC/dropbear:$SRC/dropbear.map
dropbearkey:$SRC/dropbearkey:$SRC/dropbearkey.map
sshd-console:$BUILD/sshd-console:$BUILD/sshd-console.map
sshpasswd:$BUILD/sshpasswd:$BUILD/sshpasswd.map
sshd-startup:$BUILD/sshd-startup:$BUILD/sshd-startup.map
sftp-server:$BUILD/sftp-server:$BUILD/sftp-server.map"

# ---- 8. check the unstripped programs -------------------------------------------

# None may contain fork(), vfork(), the exec family or pipe(): libnix's
# fork() returns 0 as if the caller were the child
# (sources/nix20/extra/system.c), and AmigaOS has none of the others. nm
# lists the symbols a linked program contains; a referenced libnix
# function is linked in and therefore listed.
# None may contain link() or symlink() either: libnix's link() makes a soft
# link (sources/nix20/misc/symlink.c), which broke dropbearkey's key files
# until patch 0009.
# None may contain libnix's select.o or fcntl.o (unusable for a server, see
# port/amiga_net.c), getpwent.o or __inituser.o (they call
# usergroup.library, see port/amiga_user.c), or system.o (fork, execv).
step "Checking the programs"
while IFS=: read -r name prog map; do
	syms=$("$NM" "$prog" | awk '{ print $NF }' \
		| grep -E '^_(fork|vfork|execv|execve|execvp|execl|execlp|execle|pipe|link|symlink)$' || true)
	[ -z "$syms" ] || fail "$name contains $(echo $syms)"
	objs=$(grep -oE '\((select|fcntl|getpwent|__inituser|system)\.o\)' "$map" \
		| sort -u || true)
	[ -z "$objs" ] || fail "$name links libnix's $(echo $objs)"
	echo "    $name: no fork/exec/pipe/link, no libnix select/fcntl/getpwent/__inituser/system"
done <<EOF
$PROGRAMS
EOF

# ---- 9. install into dist/ ----------------------------------------------------------

# Stripped copies; the other files in dist/ are not touched.
step "Installing into dist/"
mkdir -p "$DIST"
while IFS=: read -r name prog map; do
	"$STRIP" -o "$DIST/$name" "$prog"
	echo "    dist/$name ($(wc -c < "$DIST/$name" | tr -d ' ') bytes)"
done <<EOF
$PROGRAMS
EOF

# ---- 10. no local paths or user names in dist/ ---------------------------------------

# Every file in dist/ (programs, scripts, licences) is searched for the
# builder's home directory and user name as plain bytes.
step "Checking dist/ for local paths and user names"
USERNAME=$(id -un)
for f in "$DIST"/*; do
	[ -f "$f" ] || continue
	if LC_ALL=C grep -a -F -q -e "$HOME" "$f"; then
		fail "$f contains the home directory path"
	fi
	if LC_ALL=C grep -a -F -q -e "$USERNAME" "$f"; then
		fail "$f contains the user name"
	fi
done
echo "    none found"

step "Done"
ls -l "$DIST"
