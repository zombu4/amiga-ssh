/*
 * amiga_crypt.c - crypt() for the AmigaOS port: SHA-256 crypt ("$5$").
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * AmigaOS has no crypt(). Dropbear checks a password with
 * crypt(password, stored_hash) and compares the result with the stored hash
 * (svr-authpasswd.c:70-71); a NULL result means the account is locked.
 *
 * This is the SHA-256 variant of the SHA-crypt scheme, as specified in
 * "Unix crypt using SHA-256 and SHA-512" (U. Drepper, version 0.6, 2016),
 * the "$5$" format of glibc and of "openssl passwd -5". Its output is
 *
 *   $5$<salt>$<43 characters>
 *   $5$rounds=<n>$<salt>$<43 characters>
 *
 * The salt is at most 16 characters; the default is 5000 rounds, the
 * allowed range 1000 to 999999999. SHA-256 comes from the libtomcrypt that
 * Dropbear bundles. Any setting that is not a "$5$" hash (for example "*")
 * returns NULL, which locks password login for that account.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "tomcrypt.h"
#include "amiga_crypt.h"

#define PREFIX "$5$"
#define ROUNDS_PREFIX "rounds="
#define SALT_MAX 16
#define ROUNDS_DEFAULT 5000
#define ROUNDS_MIN 1000
#define ROUNDS_MAX 999999999UL
#define HASH_LEN 32

/* Size of the longest result, defined in amiga_crypt.h; checked here
 * against the parts it is made of. */
#define RESULT_MAX AMIGA_CRYPT_RESULT_MAX
_Static_assert(RESULT_MAX == 3 + 7 + 9 + 1 + SALT_MAX + 1 + 43 + 1,
	"AMIGA_CRYPT_RESULT_MAX does not match the $5$ format");

static const char b64[] =
	"./0123456789ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz";

/* memset through a volatile pointer, so clearing secrets is not removed
 * as a dead store. */
static void burn(void *buf, size_t len)
{
	volatile unsigned char *v = buf;

	while (len-- > 0)
		*v++ = 0;
}

static void sha_add(hash_state *hs, const void *data, size_t len)
{
	sha256_process(hs, (const unsigned char *)data, (unsigned long)len);
}

/* Adds len bytes taken from the repeated 32-byte block. */
static void add_repeated(hash_state *hs, const unsigned char *block, size_t len)
{
	for (; len >= HASH_LEN; len -= HASH_LEN)
		sha_add(hs, block, HASH_LEN);
	sha_add(hs, block, len);
}

/* Writes n base-64 characters of the 24-bit value b2:b1:b0, low bits
 * first, as the specification's b64_from_24bit does. */
static char *put24(char *out, unsigned b2, unsigned b1, unsigned b0, int n)
{
	unsigned long w = ((unsigned long)b2 << 16) | ((unsigned long)b1 << 8) | b0;

	while (n-- > 0) {
		*out++ = b64[w & 0x3f];
		w >>= 6;
	}
	return out;
}

char *amiga_sha256_crypt(const char *key, const char *setting,
		char *result, size_t size)
{
	unsigned char a[HASH_LEN], b[HASH_LEN], c[HASH_LEN], dp[HASH_LEN], ds[HASH_LEN];
	unsigned char *p = NULL, *s = NULL;
	const char *salt;
	size_t klen, slen, i;
	unsigned long rounds = ROUNDS_DEFAULT;
	int rounds_given = 0;
	hash_state hs;
	char *out;

	if (key == NULL || setting == NULL || size < RESULT_MAX
	    || strncmp(setting, PREFIX, 3) != 0)
		return NULL;
	salt = setting + 3;
	if (strncmp(salt, ROUNDS_PREFIX, 7) == 0) {
		char *end;
		unsigned long r = strtoul(salt + 7, &end, 10);

		if (end == salt + 7 || *end != '$')
			return NULL;
		rounds = r < ROUNDS_MIN ? ROUNDS_MIN : r > ROUNDS_MAX ? ROUNDS_MAX : r;
		rounds_given = 1;
		salt = end + 1;
	}
	slen = strcspn(salt, "$");
	if (slen > SALT_MAX)
		slen = SALT_MAX;
	klen = strlen(key);

	/* Digest B = SHA256(key, salt, key). */
	sha256_init(&hs);
	sha_add(&hs, key, klen);
	sha_add(&hs, salt, slen);
	sha_add(&hs, key, klen);
	sha256_done(&hs, b);

	/* Digest A = SHA256(key, salt, B repeated over klen bytes, then for
	 * each bit of klen from the lowest: B if set, the key if clear). */
	sha256_init(&hs);
	sha_add(&hs, key, klen);
	sha_add(&hs, salt, slen);
	add_repeated(&hs, b, klen);
	for (i = klen; i > 0; i >>= 1) {
		if (i & 1)
			sha_add(&hs, b, HASH_LEN);
		else
			sha_add(&hs, key, klen);
	}
	sha256_done(&hs, a);

	/* P: SHA256 of the key repeated klen times, stretched to klen bytes. */
	sha256_init(&hs);
	for (i = 0; i < klen; i++)
		sha_add(&hs, key, klen);
	sha256_done(&hs, dp);
	p = malloc(klen + 1);
	/* S: SHA256 of the salt repeated 16 + A[0] times, stretched to slen. */
	sha256_init(&hs);
	for (i = 0; i < 16u + a[0]; i++)
		sha_add(&hs, salt, slen);
	sha256_done(&hs, ds);
	s = malloc(slen + 1);
	if (p == NULL || s == NULL) {
		free(p);
		free(s);
		return NULL;
	}
	for (i = 0; i < klen; i++)
		p[i] = dp[i % HASH_LEN];
	for (i = 0; i < slen; i++)
		s[i] = ds[i % HASH_LEN];

	/* The rounds. */
	memcpy(c, a, HASH_LEN);
	for (i = 0; i < rounds; i++) {
		sha256_init(&hs);
		if (i & 1)
			sha_add(&hs, p, klen);
		else
			sha_add(&hs, c, HASH_LEN);
		if (i % 3 != 0)
			sha_add(&hs, s, slen);
		if (i % 7 != 0)
			sha_add(&hs, p, klen);
		if (i & 1)
			sha_add(&hs, c, HASH_LEN);
		else
			sha_add(&hs, p, klen);
		sha256_done(&hs, c);
	}

	/* "$5$[rounds=n$]salt$" then the digest in the scheme's byte order. */
	if (rounds_given)
		out = result + sprintf(result, "%s%s%lu$", PREFIX, ROUNDS_PREFIX, rounds);
	else
		out = result + sprintf(result, "%s", PREFIX);
	memcpy(out, salt, slen);
	out += slen;
	*out++ = '$';
	out = put24(out, c[0], c[10], c[20], 4);
	out = put24(out, c[21], c[1], c[11], 4);
	out = put24(out, c[12], c[22], c[2], 4);
	out = put24(out, c[3], c[13], c[23], 4);
	out = put24(out, c[24], c[4], c[14], 4);
	out = put24(out, c[15], c[25], c[5], 4);
	out = put24(out, c[6], c[16], c[26], 4);
	out = put24(out, c[27], c[7], c[17], 4);
	out = put24(out, c[18], c[28], c[8], 4);
	out = put24(out, c[9], c[19], c[29], 4);
	out = put24(out, 0, c[31], c[30], 3);
	*out = '\0';

	/* Clear everything derived from the password. */
	burn(a, sizeof(a));
	burn(b, sizeof(b));
	burn(c, sizeof(c));
	burn(dp, sizeof(dp));
	burn(ds, sizeof(ds));
	burn(p, klen);
	burn(s, slen);
	burn(&hs, sizeof(hs));
	free(p);
	free(s);
	return result;
}

char *crypt(const char *key, const char *setting)
{
	static char result[RESULT_MAX];

	return amiga_sha256_crypt(key, setting, result, sizeof(result));
}

void amiga_make_salt(char salt[AMIGA_SALT_LEN + 1],
		const unsigned char random[AMIGA_SALT_LEN])
{
	int i;

	for (i = 0; i < AMIGA_SALT_LEN; i++)
		salt[i] = b64[random[i] & 0x3f];
	salt[AMIGA_SALT_LEN] = '\0';
}
