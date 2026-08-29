/*
 * Shared helpers for emitting Canon IVEC command blocks.
 *
 * Copyright (c) 2026.  Licensed under the MIT License.
 */
#ifndef IVEC_H
#define IVEC_H

#include <stdio.h>

/* Opening boilerplate for a command block.  The caller appends the operation
 * name, then the param_set, then closes with IVEC_TAIL.
 *
 * Two variants because Canon uses two.  Which one a given block gets is not
 * derivable from a rule - the maintenance SetJobConfiguration declares vcn
 * without using it, while the print SetJobConfiguration does not declare it
 * at all.  Each emitter therefore reproduces what Canon actually sends for
 * that block rather than applying a tidier convention of our own. */
extern const char *IVEC_HEAD;      /* declares ivec only */
extern const char *IVEC_HEAD_VCN;  /* declares ivec and vcn */
extern const char *IVEC_TAIL;

void ivec_emit(FILE *out, const char *fmt, ...)
    __attribute__((format(printf, 2, 3)));

/* Canon writes empty elements self-closed (<ivec:jobname/>).  Mirror that
 * rather than sending <tag></tag>: the two are equivalent XML, but there is
 * no reason to hand the firmware a form its own driver never produces. */
void ivec_element(FILE *out, const char *tag, const char *value);

/* Same, in the vcn namespace.  The prefix is not cosmetic: to an XML parser
 * vcn:host_environment and ivec:host_environment are different elements. */
void vcn_element(FILE *out, const char *tag, const char *value);

/* Emits <ivec:tag><![CDATA[value]]></ivec:tag>, splitting any "]]>" in the
 * value so it cannot terminate the section early. */
void ivec_cdata_element(FILE *out, const char *tag, const char *value);

/* Local time as YYYYMMDDhhmmss, the format SetJobConfiguration expects. */
void ivec_datetime(char *buf, size_t len);

/* Canon sends the job id zero-padded to 8 digits ("00000002"), never the bare
 * number.  Width-sensitive firmware is a cheap thing to guard against. */
void ivec_jobid(char *buf, size_t len, const char *cups_job_id);

/* Canon puts a UUID in job_description.  CUPS hands us the real one in the
 * options as job-uuid=urn:uuid:...; returns NULL when it is absent. */
const char *ivec_job_uuid(int num_options, void *options);

#endif /* IVEC_H */
