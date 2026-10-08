/*
 * amiga_crypt.h - crypt() for the AmigaOS port: SHA-256 crypt ("$5$").
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#ifndef AMIGA_CRYPT_H
#define AMIGA_CRYPT_H

#include <stddef.h>

/* Length of a salt made by amiga_make_salt(). */
#define AMIGA_SALT_LEN 16

/* Longest result: "$5$rounds=999999999$" (20) + 16 salt + "$" + 43 + NUL. */
#define AMIGA_CRYPT_RESULT_MAX 81

/* SHA-256 crypt of key with the "$5$..." setting (a salt or a full stored
 * hash), written to result (size at least AMIGA_CRYPT_RESULT_MAX bytes).
 * Returns result, or NULL if setting is not a "$5$" setting or size is too
 * small. */
char *amiga_sha256_crypt(const char *key, const char *setting,
		char *result, size_t size);

/* crypt() as Dropbear calls it; the result lives in a static buffer. */
char *crypt(const char *key, const char *setting);

/* Turns AMIGA_SALT_LEN random bytes into a salt of crypt's alphabet. */
void amiga_make_salt(char salt[AMIGA_SALT_LEN + 1],
		const unsigned char random[AMIGA_SALT_LEN]);

#endif /* AMIGA_CRYPT_H */
