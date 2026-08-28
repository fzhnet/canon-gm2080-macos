/*
 * Shared helpers for emitting Canon IVEC command blocks.
 *
 * Copyright (c) 2026.  Licensed under the MIT License.
 */
#include "ivec.h"

#include <stdarg.h>
#include <time.h>

const char *IVEC_HEAD =
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
    "<cmd xmlns:ivec=\"http://www.canon.com/ns/cmd/2008/07/common/\""
    " xmlns:vcn=\"http://www.canon.com/ns/cmd/2008/07/canon/\">"
    "<ivec:contents><ivec:operation>";

const char *IVEC_TAIL = "</ivec:param_set></ivec:contents></cmd>";

void
ivec_emit(FILE *out, const char *fmt, ...)
{
    va_list ap;

    va_start(ap, fmt);
    vfprintf(out, fmt, ap);
    va_end(ap);
}

void
ivec_element(FILE *out, const char *tag, const char *value)
{
    if (value && *value)
        fprintf(out, "<ivec:%s>%s</ivec:%s>", tag, value, tag);
    else
        fprintf(out, "<ivec:%s/>", tag);
}

void
ivec_datetime(char *buf, size_t len)
{
    time_t    now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    strftime(buf, len, "%Y%m%d%H%M%S", &tm);
}
