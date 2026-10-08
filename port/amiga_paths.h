/*
 * amiga_paths.h - where the AmigaOS port keeps its files.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Every path is a native AmigaDOS path. Unix-style paths are never used:
 * with the NOIXPATHS variable set, libnix turns "/etc/x" into the volume
 * "etc:", and DOS would then wait for that volume to be inserted.
 */
#ifndef AMIGA_PATHS_H
#define AMIGA_PATHS_H

/* Directory that holds every file of the server, next to the TCP/IP
 * configuration in DEVS:Internet. */
#define AMIGA_SSHD_DIR "DEVS:Internet/sshd"

/* Login name and home directory, written by the installer. */
#define AMIGA_SSHD_CONFIG AMIGA_SSHD_DIR "/config"

/* Saved entropy pool. */
#define AMIGA_SSHD_SEED AMIGA_SSHD_DIR "/random_seed"

/* Log of the listener and every session (connections, logins, ends). When
 * it passes AMIGA_SSHD_LOG_MAX bytes it is renamed to AMIGA_SSHD_LOG_OLD,
 * replacing the previous one, so both together stay near twice that size. */
#define AMIGA_SSHD_LOG     AMIGA_SSHD_DIR "/log"
#define AMIGA_SSHD_LOG_OLD AMIGA_SSHD_DIR "/log.old"
#define AMIGA_SSHD_LOG_MAX 16384

/* Start-up script of the shells started for SSH sessions (NewShell's FROM
 * argument), installed by Install-SSH. It runs S:Shell-Startup and adds
 * the aliases exit and logout. Without it, sessions use S:Shell-Startup as
 * every other shell does. */
#define AMIGA_SSHD_SHELL_STARTUP AMIGA_SSHD_DIR "/shell-startup"

#endif /* AMIGA_PATHS_H */
