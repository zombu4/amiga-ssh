/*
 * amiga_log.h - the server's log file on AmigaOS.
 *
 * Copyright (c) 2026 zombu2
 * SPDX-License-Identifier: PolyForm-Noncommercial-1.0.0
 */
#ifndef AMIGA_LOG_H
#define AMIGA_LOG_H

/* Appends one line (without its line end) to the log file, renaming a full
 * log to log.old first. Never fails visibly: a line that cannot be written
 * is dropped. */
void amiga_log_line(const char *line);

#endif /* AMIGA_LOG_H */
