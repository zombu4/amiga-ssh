/*
 * amiga_rewrite.h - read a small text file whole and replace it safely.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 *
 * Shared by sshpasswd (port/amiga_passwd.c) and sshd-startup
 * (port/amiga_startup.c). Both change one part of a text file and must
 * never leave a half-written file behind; see amiga_rewrite.c for how the
 * replacement is done with dos.library calls only.
 */
#ifndef AMIGA_REWRITE_H
#define AMIGA_REWRITE_H

/* Largest file either program will read. Both files it is used on (the
 * server config and S:User-Startup) are far smaller; anything larger is
 * refused with ERROR_OBJECT_TOO_LARGE instead of filling memory. */
#define AMIGA_REWRITE_MAX (512L * 1024L)

/* Results of amiga_file_read() and amiga_file_replace(). */
#define AMIGA_RW_OK       0	/* done */
#define AMIGA_RW_MISSING  1	/* read: the file does not exist */
#define AMIGA_RW_STALE    2	/* replace: done, but "<name>.old" could
				 * not be deleted afterwards */
#define AMIGA_RW_ERROR   -1	/* failed; IoErr() holds the DOS error */

/* Clears len bytes at buf through a volatile pointer, so the compiler
 * cannot drop the stores as dead (same method as amiga_crypt.c's burn()). */
void amiga_burn(void *buf, unsigned long len);

/* Reads the whole file "name" into a new buffer, NUL-terminated after the
 * last byte (the NUL is not counted in *len). Returns AMIGA_RW_OK with
 * *data and *len set, AMIGA_RW_MISSING with *data = NULL and *len = 0, or
 * AMIGA_RW_ERROR.
 *
 * Crash recovery: if "name" is missing but "name.old" exists, an earlier
 * amiga_file_replace() was interrupted after it moved the old file aside
 * and before the new one took its place. The old file is then renamed back
 * first and *restored is set to 1 (otherwise 0), so the caller can say so.
 * restored may be NULL. */
int amiga_file_read(const char *name, char **data, long *len, int *restored);

/* Frees a buffer from amiga_file_read(); with wipe nonzero the contents
 * are cleared first. data may be NULL. */
void amiga_file_free(char *data, long len, int wipe);

/* Replaces the file "name" with len bytes at data:
 *  1. writes them to "name.new" in the same directory and closes it,
 *  2. gives it the old file's protection bits and comment,
 *  3. renames the old file to "name.old",
 *  4. renames "name.new" to "name",
 *  5. deletes "name.old".
 * dos.library's Rename() refuses to overwrite an existing name, which is
 * why step 3 exists. At every moment either "name" or "name.old" is a
 * complete file. If step 4 fails, step 3 is undone. Returns AMIGA_RW_OK,
 * AMIGA_RW_STALE (step 5 failed; the new file is in place) or
 * AMIGA_RW_ERROR (nothing changed; IoErr() holds the reason). */
int amiga_file_replace(const char *name, const char *data, long len);

#endif /* AMIGA_REWRITE_H */
