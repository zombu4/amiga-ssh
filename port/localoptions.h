/*
 * localoptions.h - Dropbear build options for the AmigaOS port.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * build.sh copies this file into Dropbear's build directory. Dropbear's
 * options.h includes it after config.h and before default_options.h, so
 * every setting here overrides the default of the same name. Line numbers
 * refer to Dropbear's src/default_options.h at the pinned commit.
 */
#ifndef AMIGA_LOCALOPTIONS_H
#define AMIGA_LOCALOPTIONS_H

#include "amiga_paths.h"

/* ---- functions the port supplies (configure cannot see them) ---------- */

/* crypt(): SHA-256 crypt in port/amiga_crypt.c. sysoptions.h refuses
 * password login without it. */
#define HAVE_CRYPT 1

/* setusershell()/getusershell()/endusershell() in port/amiga_user.c;
 * Dropbear's compat.c versions read the Unix path /etc/shells. */
#define HAVE_GETUSERSHELL 1

/* ---- process model ---------------------------------------------------- */

/* The port has its own listener (port/amiga_main.c), which starts one
 * session process per connection. Dropbear's listener (main_noinetd) needs
 * fork() and is left out; each session runs Dropbear's inetd mode. */
#define NON_INETD_MODE 0
#define INETD_MODE 1

/* Re-exec is Linux-only anyway (sysoptions.h:45-49). */
#define DROPBEAR_REEXEC 0

/* Unauthenticated connections the listener (port/amiga_main.c) allows at
 * once, in total and per client address; upstream's defaults are 30 and 5
 * (default_options.h:302, 306). Each connection is a separate copy of the
 * program with its own 128 KB stack, so the limits are kept to what a
 * small Amiga can hold. */
#define MAX_UNAUTH_CLIENTS 4
#define MAX_UNAUTH_PER_IP 2

/* ---- accounts --------------------------------------------------------- */

/* AmigaOS has no users: one account, from port/amiga_user.c. */
#define DROPBEAR_SVR_MULTIUSER 0
#define DROPBEAR_SVR_DROP_PRIVS 0

/* Password login, checked against the SHA-256 crypt hash in the config
 * file, and public-key login. */
#define DROPBEAR_SVR_PASSWORD_AUTH 1
#define DROPBEAR_SVR_PUBKEY_AUTH 1

/* ---- features that need what AmigaOS lacks ---------------------------- */

/* Agent and stream forwarding use Unix-domain sockets. */
#define DROPBEAR_SVR_AGENTFWD 0
#define DROPBEAR_SVR_LOCALSTREAMFWD 0
#define DROPBEAR_SVR_REMOTESTREAMFWD 0

/* No X server. */
#define DROPBEAR_X11FWD 0

/* No sftp-server program exists for AmigaOS. */
#define DROPBEAR_SFTPSERVER 0

/* The message of the day is a Unix file (/etc/motd). */
#define DO_MOTD 0

/* ---- files ------------------------------------------------------------ */

#define DSS_PRIV_FILENAME     AMIGA_SSHD_DIR "/dropbear_dss_host_key"
#define RSA_PRIV_FILENAME     AMIGA_SSHD_DIR "/dropbear_rsa_host_key"
#define ECDSA_PRIV_FILENAME   AMIGA_SSHD_DIR "/dropbear_ecdsa_host_key"
#define ED25519_PRIV_FILENAME AMIGA_SSHD_DIR "/dropbear_ed25519_host_key"

/* Dropbear reads its random device at start and writes fresh output back
 * (dbrandom.c:223-310); on AmigaOS that device is the seed file. */
#define DROPBEAR_URANDOM_DEV AMIGA_SSHD_SEED

/* Not written in inetd mode; set to an AmigaDOS path so no Unix path
 * remains in the program. */
#define DROPBEAR_PIDFILE "T:sshd.pid"

#endif /* AMIGA_LOCALOPTIONS_H */
