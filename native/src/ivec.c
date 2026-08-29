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

/* Element values carry a job title and a user name, which are whatever the
 * person printing typed.  A document called "P&L <draft>.pdf" would otherwise
 * emit malformed XML, and a crafted title could close jobname early and inject
 * elements into the command stream.  Escaping lives here, in the only place
 * that writes element text, so no caller can forget it. */
static void
emit_escaped(FILE *out, const char *s)
{
    for (; *s; s ++) {
        switch (*s) {
        case '&':  fputs("&amp;",  out); break;
        case '<':  fputs("&lt;",   out); break;
        case '>':  fputs("&gt;",   out); break;
        case '"':  fputs("&quot;", out); break;
        case '\'': fputs("&apos;", out); break;
        default:
            /* Control characters are illegal in XML 1.0 even as entities, so
             * they are dropped rather than escaped. */
            if ((unsigned char)*s >= 0x20 || *s == '\t')
                fputc(*s, out);
            break;
        }
    }
}

static void
element(FILE *out, const char *ns, const char *tag, const char *value)
{
    if (value && *value) {
        fprintf(out, "<%s:%s>", ns, tag);
        emit_escaped(out, value);
        fprintf(out, "</%s:%s>", ns, tag);
    } else {
        fprintf(out, "<%s:%s/>", ns, tag);
    }
}

void
ivec_element(FILE *out, const char *tag, const char *value)
{
    element(out, "ivec", tag, value);
}

void
vcn_element(FILE *out, const char *tag, const char *value)
{
    element(out, "vcn", tag, value);
}

void
ivec_cdata_element(FILE *out, const char *tag, const char *value)
{
    const char *p = value ? value : "";

    fprintf(out, "<ivec:%s><![CDATA[", tag);

    /* "]]>" cannot appear inside a CDATA section: it would end it early and
     * let the remainder be parsed as markup.  The standard escape is to close
     * the section and reopen it mid-sequence. */
    while (*p) {
        if (p[0] == ']' && p[1] == ']' && p[2] == '>') {
            fputs("]]]]><![CDATA[>", out);
            p += 3;
        } else {
            fputc(*p ++, out);
        }
    }

    fprintf(out, "]]></ivec:%s>", tag);
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

    /* Canon's field is exactly 8 digits.  A CUPS id can legitimately grow
     * past that (and strtol saturates to LONG_MAX on garbage input), so wrap
     * rather than widen the field. */
    snprintf(buf, len, "%08ld", id % 100000000L);
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
