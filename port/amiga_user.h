/*
 * amiga_user.h - the single account of the AmigaOS port.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#ifndef AMIGA_USER_H
#define AMIGA_USER_H

/* The account's home directory from the config file, or NULL if the config
 * file is missing or names no user. */
const char *amiga_user_home(void);

/* Valid-shell list used by Dropbear's checkusername(); declared here because
 * the build defines HAVE_GETUSERSHELL, which hides Dropbear's own
 * declarations in compat.h. */
void setusershell(void);
char *getusershell(void);
void endusershell(void);

#endif /* AMIGA_USER_H */
