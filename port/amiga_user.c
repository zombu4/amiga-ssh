/*
 * amiga_user.c - the single account of the AmigaOS port.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * AmigaOS has no users. The server is built with DROPBEAR_SVR_MULTIUSER 0
 * and knows exactly one account: the login name and home directory written
 * by the installer to AMIGA_SSHD_CONFIG. Every key in authorized_keys logs
 * in as that account.
 *
 * libnix's own user functions cannot be used. getpwnam() and getgroups()
 * call usergroup.library whenever a bsdsocket.library stack is running
 * (sources/socket/unistd/getpwent.c, __inituser.c), and that library is
 * absent on most systems; getuid() and friends all return 0
 * (sources/nix/extra/getppid.c). This file defines every one of them, so
 * none of those libnix objects is linked.
 *
 * The config file has one setting per line, "name=value". Lines starting
 * with '#' and empty lines are ignored:
 *
 *   user=amiga
 *   home=SYS:
 *   password=$5$<salt>$<hash>
 *
 * The password line holds a SHA-256 crypt hash (port/amiga_crypt.c). Without
 * it, password login is locked and only keys work.
 */
#include <errno.h>
#include <pwd.h>
#include <stdio.h>
#include <string.h>
#include <sys/types.h>
#include <unistd.h>

#include "amiga_paths.h"
#include "amiga_user.h"

/* Fixed ids of the one account. Not 0, so nothing treats it as root. */
#define AMIGA_UID 100
#define AMIGA_GID 100

/* What the account's shell is called. Dropbear only compares it with the
 * list from getusershell(); the session code starts the AmigaDOS shell
 * itself. */
#define AMIGA_SHELL "NewShell"

#define NAME_MAX_LEN 32
#define HOME_MAX_LEN 256
#define PASS_MAX_LEN 127

static char user_name[NAME_MAX_LEN + 1];
static char user_home[HOME_MAX_LEN + 1];
static char user_pass[PASS_MAX_LEN + 1];
static int config_state;	/* 0 not read, 1 read and valid, -1 unusable */
static struct passwd account;

/* Copies value into dst (size bytes) without the line end. Returns 0 if it
 * fits and is not empty, -1 otherwise. */
static int take_value(char *dst, size_t size, const char *value)
{
	size_t len = strcspn(value, "\r\n");

	if (len == 0 || len >= size)
		return -1;
	memcpy(dst, value, len);
	dst[len] = '\0';
	return 0;
}

static int read_config(void)
{
	char line[HOME_MAX_LEN + PASS_MAX_LEN + 16];
	FILE *f;
	int ok = 0, continuation = 0;

	if (config_state != 0)
		return config_state;

	config_state = -1;
	strcpy(user_home, "SYS:");
	f = fopen(AMIGA_SSHD_CONFIG, "r");
	if (f == NULL)
		return config_state;
	while (fgets(line, sizeof(line), f) != NULL) {
		size_t n = strlen(line);
		int complete = n > 0 && line[n - 1] == '\n';
		/* A line longer than the buffer arrives in pieces. It is ignored
		 * whole: its first piece (no line end, not at end of file) and
		 * every following piece up to its line end. A last line without a
		 * line end at the end of the file is read normally. */
		int skip = continuation || (!complete && !feof(f));

		continuation = !complete;
		if (skip)
			continue;
		if (line[0] == '#' || line[0] == '\n' || line[0] == '\r')
			continue;
		if (strncmp(line, "user=", 5) == 0)
			ok = take_value(user_name, sizeof(user_name), line + 5) == 0;
		else if (strncmp(line, "home=", 5) == 0) {
			if (take_value(user_home, sizeof(user_home), line + 5) != 0)
				strcpy(user_home, "SYS:");
		} else if (strncmp(line, "password=", 9) == 0) {
			if (take_value(user_pass, sizeof(user_pass), line + 9) != 0)
				user_pass[0] = '\0';
		}
	}
	fclose(f);
	if (!ok)
		return config_state;

	memset(&account, 0, sizeof(account));
	account.pw_name = user_name;
	/* The stored hash, or "*" (locked) without one. Never empty: an empty
	 * password would let the "none" login method succeed if the server
	 * were started with -B (svr-auth.c:120-128). */
	account.pw_passwd = user_pass[0] != '\0' ? user_pass : "*";
	account.pw_uid = AMIGA_UID;
	account.pw_gid = AMIGA_GID;
	account.pw_gecos = user_name;
	account.pw_dir = user_home;
	account.pw_shell = AMIGA_SHELL;
	config_state = 1;
	return config_state;
}

const char *amiga_user_home(void)
{
	return read_config() == 1 ? user_home : NULL;
}

struct passwd *getpwnam(const char *name)
{
	if (name == NULL || read_config() != 1 || strcmp(name, user_name) != 0)
		return NULL;
	return &account;
}

struct passwd *getpwuid(uid_t uid)
{
	if (uid != AMIGA_UID || read_config() != 1)
		return NULL;
	return &account;
}

uid_t getuid(void)
{
	return AMIGA_UID;
}

uid_t geteuid(void)
{
	return AMIGA_UID;
}

gid_t getgid(void)
{
	return AMIGA_GID;
}

gid_t getegid(void)
{
	return AMIGA_GID;
}

/* AmigaOS has no groups. A single-user Dropbear refuses to start unless
 * this fails with ENOSYS (common-session.c:71-82). */
int getgroups(int size, gid_t list[])
{
	(void)size;
	(void)list;
	errno = ENOSYS;
	return -1;
}

/* Switching to the account the server already is succeeds; anything else
 * is refused, since there is nothing to switch to. */
int setuid(uid_t uid)
{
	if (uid != AMIGA_UID) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

int seteuid(uid_t uid)
{
	return setuid(uid);
}

int setgid(gid_t gid)
{
	if (gid != AMIGA_GID) {
		errno = EPERM;
		return -1;
	}
	return 0;
}

int setegid(gid_t gid)
{
	return setgid(gid);
}

int initgroups(const char *name, gid_t basegid)
{
	(void)name;
	return setgid(basegid);
}

/* The list of valid shells (Dropbear's checkusername, svr-auth.c:309-335):
 * exactly the account's shell. Replaces Dropbear's compat.c version, which
 * reads the Unix path /etc/shells; the build defines HAVE_GETUSERSHELL. */
static int shell_index;

void setusershell(void)
{
	shell_index = 0;
}

char *getusershell(void)
{
	if (shell_index++ == 0)
		return AMIGA_SHELL;
	return NULL;
}

void endusershell(void)
{
	shell_index = 0;
}
