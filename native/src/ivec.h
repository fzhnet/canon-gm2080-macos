/*
 * Shared helpers for emitting Canon IVEC command blocks.
 *
 * Copyright (c) 2026.  Licensed under the MIT License.
 */
#ifndef IVEC_H
#define IVEC_H

#include <stdio.h>

/* Opening boilerplate shared by every command block.  The caller appends the
 * operation name, then the param_set, then closes with IVEC_TAIL. */
extern const char *IVEC_HEAD;
extern const char *IVEC_TAIL;

void ivec_emit(FILE *out, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Canon writes empty elements self-closed (<ivec:jobname/>).  Mirror that
 * rather than sending <tag></tag>: the two are equivalent XML, but there is
 * no reason to hand the firmware a form its own driver never produces. */
void ivec_element(FILE *out, const char *tag, const char *value);

/* Local time as YYYYMMDDhhmmss, the format SetJobConfiguration expects. */
void ivec_datetime(char *buf, size_t len);

#endif /* IVEC_H */
