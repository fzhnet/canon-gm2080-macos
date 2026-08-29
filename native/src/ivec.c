/*
 * Shared helpers for emitting Canon IVEC command blocks.
 *
 * Copyright (c) 2026.  Licensed under the MIT License.
 */
#include "ivec.h"

#include <stdarg.h>
#include <time.h>
#include <stdlib.h>
#include <string.h>
#include <cups/cups.h>

const char *IVEC_HEAD =
    "<?xml version=\"1.0\" encoding=\"utf-8\" ?>"
    "<cmd xmlns:ivec=\"http://www.canon.com/ns/cmd/2008/07/common/\">"
    "<ivec:contents><ivec:operation>";

const char *IVEC_HEAD_VCN =
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
vcn_element(FILE *out, const char *tag, const char *value)
{
    if (value && *value)
        fprintf(out, "<vcn:%s>%s</vcn:%s>", tag, value, tag);
    else
        fprintf(out, "<vcn:%s/>", tag);
}

void
ivec_datetime(char *buf, size_t len)
{
    time_t    now = time(NULL);
    struct tm tm;

    localtime_r(&now, &tm);
    strftime(buf, len, "%Y%m%d%H%M%S", &tm);
}

void
ivec_jobid(char *buf, size_t len, const char *cups_job_id)
{
    long id = cups_job_id ? strtol(cups_job_id, NULL, 10) : 0;

    if (id < 0)
        id = 0;

    snprintf(buf, len, "%08ld", id);
}

const char *
ivec_job_uuid(int num_options, void *options)
{
    const char *uuid = cupsGetOption("job-uuid", num_options,
                                     (cups_option_t *)options);

    if (!uuid)
        return NULL;

    /* CUPS spells it as a URN; Canon sends the bare UUID. */
    if (!strncmp(uuid, "urn:uuid:", 9))
        uuid += 9;

    return *uuid ? uuid : NULL;
}
