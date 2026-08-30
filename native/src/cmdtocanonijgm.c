/*
 * cmdtocanonijgm - CUPS command filter for Canon GM2000/GM2080 series
 *
 * Handles printer maintenance: head cleaning, nozzle check, alignment.  CUPS
 * invokes this for application/vnd.cups-command jobs, whose payload is a
 * plain-text file listing commands one per line:
 *
 *     #CUPS-COMMAND
 *     Clean all
 *
 * A maintenance job uses the same IVEC envelope as a print job but with
 * servicetype="maintenance" and no raster:
 *
 *     StartJob / SetJobConfiguration / <Cleaning|TestPrint> / EndJob
 *
 * Several details differ from the print path and are easy to get wrong: the
 * operation block lists its own parameters BEFORE jobID, and the two paths
 * disagree about which blocks declare the vcn namespace.  All of it is
 * reproduced from Canon's own output rather than from a rule.
 *
 * Copyright (c) 2026.  Licensed under the MIT License.
 * Canon, PIXMA and IVEC are trademarks of Canon Inc.; this is not a Canon
 * product and contains no Canon code.
 */

#include "ivec.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <errno.h>
#include <cups/cups.h>

/* ------------------------------------------------------------- operations */

static void
start_job(FILE *out, const char *job_id, const char *uuid)
{
    ivec_emit(out, "%sStartJob</ivec:operation>"
                   "<ivec:param_set servicetype=\"maintenance\">"
                   "<ivec:jobID>%s</ivec:jobID>"
                   "<ivec:bidi>0</ivec:bidi>",
              IVEC_HEAD_VCN, job_id);
    ivec_element(out, "jobname", NULL);
    ivec_element(out, "username", NULL);
    ivec_element(out, "computername", NULL);
    ivec_cdata_element(out, "job_description", uuid);
    vcn_element(out, "host_environment", "linux");
    ivec_emit(out, "%s", IVEC_TAIL);
}

/* Canon declares the vcn namespace on this block even though it uses no vcn
 * element, while the print path's SetJobConfiguration does not declare it at
 * all.  Reproduced rather than tidied up: matching the reference output is
 * worth more here than internal consistency we invented. */
static void
set_job_configuration(FILE *out, const char *job_id)
{
    char stamp[32];

    ivec_datetime(stamp, sizeof(stamp));
    ivec_emit(out, "%sSetJobConfiguration</ivec:operation>"
                   "<ivec:param_set servicetype=\"maintenance\">"
                   "<ivec:jobID>%s</ivec:jobID>"
                   "<ivec:datetime>%s</ivec:datetime>%s",
              IVEC_HEAD_VCN, job_id, stamp, IVEC_TAIL);
}

static void
cleaning(FILE *out, const char *job_id, const char *type, const char *inkgroup)
{
    ivec_emit(out, "%sCleaning</ivec:operation>"
                   "<ivec:param_set servicetype=\"maintenance\">"
                   "<ivec:inkgroup>%s</ivec:inkgroup>"
                   "<ivec:type>%s</ivec:type>"
                   "<ivec:jobID>%s</ivec:jobID>%s",
              IVEC_HEAD, inkgroup, type, job_id, IVEC_TAIL);
}

static void
test_print(FILE *out, const char *job_id, const char *type)
{
    ivec_emit(out, "%sTestPrint</ivec:operation>"
                   "<ivec:param_set servicetype=\"maintenance\">"
                   "<ivec:type>%s</ivec:type>"
                   "<ivec:jobID>%s</ivec:jobID>%s",
              IVEC_HEAD, type, job_id, IVEC_TAIL);
}

static void
end_job(FILE *out, const char *job_id)
{
    ivec_emit(out, "%sEndJob</ivec:operation>"
                   "<ivec:param_set servicetype=\"maintenance\">"
                   "<ivec:jobID>%s</ivec:jobID>%s",
              IVEC_HEAD, job_id, IVEC_TAIL);
}

/* ---------------------------------------------------------------- dispatch */

/* The commands this filter understands.  A table rather than a chain of ifs
 * so that resolving a command and performing it are separate steps: every
 * line in the file is resolved before any of them is sent, because a
 * half-executed maintenance job cannot be taken back.
 *
 * "Clean" and "Clean all" are both accepted: the PPD advertises the former
 * (the standard CUPS spelling) and maintenance.sh sends the latter, which is
 * what Canon's own driver accepts. */
typedef struct {
    const char *name;
    int         cleaning;         /* Cleaning if set, otherwise TestPrint */
    const char *type;
    const char *inkgroup;         /* Cleaning only */
} command_t;

static const command_t COMMANDS[] = {
    { "Clean",                   1, "regular",           "all" },
    { "Clean all",               1, "regular",           "all" },
    { "com.canon.deepclean",     1, "deep",              "all" },

    /* System cleaning - Canon's most aggressive purge, its own utility warns
     * it "consumes a large amount of ink".  "choke" sits in the same type enum
     * as regular and deep inside Canon's IVEC command bundle, but unlike those
     * two it could not be captured from a running driver: cnijfilter2 never
     * emits it, and the printer answers no queries on port 9100.  So the
     * operation and parameter shape are known exactly and only the label
     * mapping is inferred.  See docs/protocol.md. */
    { "com.canon.systemclean",   1, "choke",             "all" },

    { "PrintSelfTestPage",       0, "nozzle_check",      NULL  },
    { "com.canon.autoalignment", 0, "auto_registration", NULL  },
    { NULL, 0, NULL, NULL }
};

static const command_t *
resolve(const char *cmd)
{
    int i;

    for (i = 0; COMMANDS[i].name; i ++)
        if (!strcasecmp(cmd, COMMANDS[i].name))
            return &COMMANDS[i];

    return NULL;
}

static void
perform(FILE *out, const char *job_id, const command_t *c)
{
    if (c->cleaning)
        cleaning(out, job_id, c->type, c->inkgroup);
    else
        test_print(out, job_id, c->type);
}

/* -------------------------------------------------------------------- main */

int
main(int argc, char *argv[])
{
    FILE *in = stdin;
    char  line[1024];
    char  uuid[64], padded_id[16];
    const char *job_id, *user, *real_uuid;
    int   num_options;
    cups_option_t *options;
    int   handled = 0, unknown = 0, i;
    /* A maintenance file is a handful of lines; the cap only exists so the
     * array cannot be overrun by a pathological one. */
    const command_t *resolved[32];

    if (argc < 6 || argc > 7) {
        fputs("ERROR: cmdtocanonijgm job user title copies options [file]\n",
              stderr);
        return 1;
    }

    job_id = argv[1];
    user   = argv[2];

    if (argc == 7 && !(in = fopen(argv[6], "r"))) {
        fprintf(stderr, "ERROR: cannot open %s: %s\n",
                argv[6], strerror(errno));
        return 1;
    }

    num_options = cupsParseOptions(argv[5], 0, &options);

    if ((real_uuid = ivec_job_uuid(num_options, options)))
        snprintf(uuid, sizeof(uuid), "%s", real_uuid);
    else
        snprintf(uuid, sizeof(uuid), "%s-%s", job_id, user ? user : "cups");

    cupsFreeOptions(num_options, options);

    ivec_jobid(padded_id, sizeof(padded_id), job_id);
    job_id = padded_id;

    /* Resolve every line before sending anything.  These operations are
     * physical and not undoable: if the file were streamed as it was parsed, a
     * trailing bad line would fail the job only after the printer had already
     * run a cleaning cycle, and CUPS reporting failure invites the user to
     * resubmit and spend the ink a second time. */
    while (fgets(line, sizeof(line), in)) {
        char *p = line, *end;

        while (*p == ' ' || *p == '\t') p ++;
        end = p + strlen(p);
        while (end > p && (end[-1] == '\n' || end[-1] == '\r' ||
                           end[-1] == ' '  || end[-1] == '\t'))
            *--end = '\0';

        /* Blank lines and the #CUPS-COMMAND banner carry no instruction. */
        if (!*p || *p == '#')
            continue;

        if (!(resolved[handled] = resolve(p))) {
            fprintf(stderr, "ERROR: unsupported command: %s\n", p);
            unknown ++;
            continue;
        }

        if (++handled == (int)(sizeof(resolved) / sizeof(resolved[0]))) {
            fputs("ERROR: too many commands in one job\n", stderr);
            unknown ++;
            break;
        }
    }

    if (in != stdin)
        fclose(in);

    if (unknown) {
        fputs("ERROR: nothing was sent to the printer\n", stderr);
        return 1;
    }

    if (!handled) {
        fputs("ERROR: command file contained no commands\n", stderr);
        return 1;
    }

    /* One envelope around all of them, matching Canon. */
    start_job(stdout, job_id, uuid);
    set_job_configuration(stdout, job_id);

    for (i = 0; i < handled; i ++)
        perform(stdout, job_id, resolved[i]);

    end_job(stdout, job_id);
    fflush(stdout);

    return 0;
}
