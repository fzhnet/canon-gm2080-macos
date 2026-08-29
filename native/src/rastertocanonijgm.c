/*
 * rastertocanonijgm - native CUPS filter for Canon GM2000/GM2080 series
 *
 * Canon ships no macOS driver for these printers and they speak no driverless
 * protocol.  Their wire format turns out to be simple, though: a handful of
 * plain-text IVEC XML command blocks, concatenated, with a standard PWG Raster
 * payload sitting between the SendData command and EndJob.
 *
 *     StartJob / SetJobConfiguration / SetConfiguration / VendorCmd / SendData
 *     <PWG Raster bytes>
 *     EndJob
 *
 * There is no binary framing: no length prefixes, no checksums, no escapes.
 * So this filter needs no Canon code at all.
 *
 * The page data is converted in-process with libcups' CUPS_RASTER_WRITE_PWG
 * mode rather than by shelling out to rastertopwg.  That is deliberate:
 * rastertopwg pads the image back out to the full sheet, because full-page
 * rasters are the PWG convention, but Canon's driver sends only the imageable
 * area.  Padding it would shift every page by the margin width.
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
#include <sys/stat.h>
#include <fcntl.h>
#include <cups/cups.h>
#include <cups/raster.h>

static void
start_job(FILE *out, const char *job_id, const char *user,
          const char *title, const char *uuid)
{
    ivec_emit(out, "%sStartJob</ivec:operation>"
              "<ivec:param_set servicetype=\"print\">"
              "<ivec:jobID>%s</ivec:jobID>"
              "<ivec:bidi>0</ivec:bidi>",
         IVEC_HEAD_VCN, job_id);
    vcn_element(out, "forcepmdetection", "OFF");
    ivec_element(out, "jobname", title);
    ivec_element(out, "username", user);
    ivec_element(out, "computername", NULL);
    ivec_cdata_element(out, "job_description", uuid);
    vcn_element(out, "host_environment", "linux");
    ivec_emit(out, "%s", IVEC_TAIL);
}

static void
set_job_configuration(FILE *out, const char *job_id)
{
    char stamp[32];

    ivec_datetime(stamp, sizeof(stamp));

    ivec_emit(out, "%sSetJobConfiguration</ivec:operation>"
              "<ivec:param_set servicetype=\"print\">"
              "<ivec:jobID>%s</ivec:jobID>"
              "<ivec:mismatch_mode>none</ivec:mismatch_mode>"
              "<ivec:datetime>%s</ivec:datetime>"
              "</ivec:param_set></ivec:contents></cmd>",
         IVEC_HEAD, job_id, stamp);
}

static void
set_configuration(FILE *out, const char *job_id, const char *media,
                  const char *type, const char *colormode, int duplex)
{
    ivec_emit(out, "%sSetConfiguration</ivec:operation>"
              "<ivec:param_set servicetype=\"print\">"
              "<ivec:jobID>%s</ivec:jobID>"
              "<ivec:papersize>%s</ivec:papersize>"
              "<ivec:papertype>%s</ivec:papertype>"
              "<ivec:borderlessprint>OFF</ivec:borderlessprint>"
              "<ivec:printcolormode>%s</ivec:printcolormode>"
              "<ivec:duplexprint>%s</ivec:duplexprint>"
              "</ivec:param_set></ivec:contents></cmd>",
         IVEC_HEAD, job_id, media, type, colormode, duplex ? "ON" : "OFF");
}

static void
set_page_configuration(FILE *out, const char *job_id, int more_pages)
{
    ivec_emit(out, "%sVendorCmd</ivec:operation>"
              "<ivec:param_set servicetype=\"print\">"
              "<ivec:jobID>%s</ivec:jobID>"
              "<vcn:ijoperation>SetPageConfiguration</vcn:ijoperation>"
              "<vcn:nextpage>%s</vcn:nextpage>"
              "</ivec:param_set></ivec:contents></cmd>",
         IVEC_HEAD_VCN, job_id, more_pages ? "ON" : "OFF");
}

static void
send_data(FILE *out, const char *job_id, long size)
{
    ivec_emit(out, "%sSendData</ivec:operation>"
              "<ivec:param_set servicetype=\"print\">"
              "<ivec:jobID>%s</ivec:jobID>"
              "<ivec:format>PWGRaster</ivec:format>"
              "<ivec:datasize>%ld</ivec:datasize>"
              "</ivec:param_set></ivec:contents></cmd>",
         IVEC_HEAD, job_id, size);
}

static void
end_job(FILE *out, const char *job_id)
{
    ivec_emit(out, "%sEndJob</ivec:operation>"
              "<ivec:param_set servicetype=\"print\">"
              "<ivec:jobID>%s</ivec:jobID>"
              "</ivec:param_set></ivec:contents></cmd>",
         IVEC_HEAD, job_id);
}

/* ------------------------------------------------------------------ media */

/* PPD PageSize keyword -> the media name the printer's SetConfiguration
 * command expects.  These are mostly PWG self-describing names, but not all:
 * businesscard is a Canon-private name.  Every entry below was read back from
 * Canon's own driver rather than guessed - see docs/ for the method. */
typedef struct { const char *ppd; const char *ivec; } ivec_map_t;

static const ivec_map_t MEDIA[] = {
    { "A4",           "iso_a4_210x297mm"         },
    { "Letter",       "na_letter_8.5x11in"       },
    { "legal",        "na_legal_8.5x14in"        },
    { "A5",           "iso_a5_148x210mm"         },
    { "B5",           "jis_b5_182x257mm"         },
    { "Postcard",     "jpn_hagaki_100x148mm"     },
    { "envelop10p",   "na_number-10_4.125x9.5in" },
    { "envelopdlp",   "iso_dl_110x220mm"         },
    { "businesscard", "custom_canon_55x91mm"     },
    { NULL, NULL }
};

/* PPD MediaType keyword -> IVEC papertype.  Plain paper is the only one with
 * a standard name; the rest are Canon-private ordinals. */
static const ivec_map_t MEDIATYPE[] = {
    { "plain",      "stationery"                  },
    { "envelope",   "custom-media-type-canon-18"  },
    { "ijpostcard", "custom-media-type-canon-11"  },
    { "postcard",   "custom-media-type-canon-12"  },
    { "highres",    "custom-media-type-canon-9"   },
    { NULL, NULL }
};

static const char *
lookup(const ivec_map_t *table, const char *key, const char *fallback)
{
    int i;

    if (key)
        for (i = 0; table[i].ppd; i ++)
            if (!strcasecmp(key, table[i].ppd))
                return table[i].ivec;

    return fallback;
}

/* ------------------------------------------------------------------- main */

/* Where a page's PWG bytes live inside the single spool file. */
typedef struct {
    off_t off;
    long  size;
} page_buf_t;

/* One temp file holds every page.  cupsd exports TMPDIR pointing inside the
 * sandbox it runs filters in, so a hardcoded /private/tmp is not guaranteed
 * to be writable when the filter runs for real rather than by hand. */
static int
open_spool(void)
{
    const char *tmpdir = getenv("TMPDIR");
    char        tmpl[1024];
    int         fd;

    snprintf(tmpl, sizeof(tmpl), "%s/gmpwg.XXXXXX",
             (tmpdir && *tmpdir) ? tmpdir : "/tmp");

    if ((fd = mkstemp(tmpl)) >= 0)
        unlink(tmpl);

    return fd;
}

int
main(int argc, char *argv[])
{
    int            num_options;
    cups_option_t  *options;
    const char     *job_id, *user, *title, *val;
    const char     *media, *papertype, *colormode;
    int            duplex, infd, i;
    cups_raster_t  *ras_in;
    cups_page_header2_t header, first;
    page_buf_t     *pages = NULL;
    int            npages = 0, cap = 0, have_first = 0;
    int            spool = -1, job_open = 0;
    char           uuid[64], padded_id[16];
    const char     *real_uuid;

    if (argc < 6 || argc > 7) {
        fputs("ERROR: rastertocanonijgm job user title copies options [file]\n",
              stderr);
        return 1;
    }

    job_id = argv[1];
    user   = argv[2];
    title  = argv[3];

    num_options = cupsParseOptions(argv[5], 0, &options);

    /* The GM series is a single-black machine; colour is never meaningful. */
    colormode = "monochrome";

    if (argc == 7) {
        if ((infd = open(argv[6], O_RDONLY)) < 0) {
            fprintf(stderr, "ERROR: cannot open %s: %s\n",
                    argv[6], strerror(errno));
            return 1;
        }
    } else {
        infd = 0;
    }

    if (!(ras_in = cupsRasterOpen(infd, CUPS_RASTER_READ))) {
        fputs("ERROR: cannot read CUPS raster on input\n", stderr);
        return 1;
    }

    /* Each page becomes its own self-contained PWG stream, buffered so that
     * its SendData command can declare an exact byte count.  Canon sends one
     * VendorCmd+SendData pair per page, not one for the whole job. */
    while (cupsRasterReadHeader2(ras_in, &header)) {
        cups_raster_t *ras_out;
        unsigned char *line;
        unsigned      y;
        off_t         start, end;
        int           truncated = 0;

        /* The first page's header is the authority on how the job was
         * actually rasterised - see the duplex handling below. */
        if (!have_first) {
            first      = header;
            have_first = 1;
        }

        if (spool < 0 && (spool = open_spool()) < 0) {
            fprintf(stderr, "ERROR: cannot create spool file: %s\n",
                    strerror(errno));
            goto fail;
        }

        if ((start = lseek(spool, 0, SEEK_END)) < 0) {
            fprintf(stderr, "ERROR: cannot append to spool: %s\n",
                    strerror(errno));
            goto fail;
        }

        if (!(ras_out = cupsRasterOpen(spool, CUPS_RASTER_WRITE_PWG))) {
            fputs("ERROR: cannot open PWG raster writer\n", stderr);
            goto fail;
        }

        if (!(line = malloc(header.cupsBytesPerLine))) {
            fputs("ERROR: out of memory for raster line\n", stderr);
            cupsRasterClose(ras_out);
            goto fail;
        }

        if (!cupsRasterWriteHeader2(ras_out, &header)) {
            fputs("ERROR: cannot write PWG page header\n", stderr);
            free(line);
            cupsRasterClose(ras_out);
            goto fail;
        }

        /* Pass the page through unchanged: the incoming raster already has the
         * geometry the printer expects, so this is a container change only. */
        for (y = 0; y < header.cupsHeight; y ++) {
            if (cupsRasterReadPixels(ras_in, line, header.cupsBytesPerLine)
                    < header.cupsBytesPerLine ||
                cupsRasterWritePixels(ras_out, line, header.cupsBytesPerLine)
                    < header.cupsBytesPerLine) {
                truncated = 1;
                break;
            }
        }

        free(line);
        cupsRasterClose(ras_out);

        /* The header just written promises cupsHeight lines.  Shipping fewer
         * would leave the printer waiting mid-page for rows that never come,
         * so a short page fails the job rather than being sent as complete. */
        if (truncated) {
            fprintf(stderr,
                    "ERROR: page %d ended after %u of %u lines\n",
                    npages + 1, y, header.cupsHeight);
            goto fail;
        }

        if ((end = lseek(spool, 0, SEEK_CUR)) < 0 || end <= start) {
            fputs("ERROR: page produced no raster data\n", stderr);
            goto fail;
        }

        if (npages >= cap) {
            page_buf_t *grown;
            cap = cap ? cap * 2 : 8;
            if (!(grown = realloc(pages, (size_t)cap * sizeof(*pages)))) {
                fputs("ERROR: out of memory tracking pages\n", stderr);
                goto fail;
            }
            pages = grown;
        }

        pages[npages].off  = start;
        pages[npages].size = (long)(end - start);
        npages ++;

        fprintf(stderr, "PAGE: %d 1\n", npages);
    }

    cupsRasterClose(ras_in);

    if (npages == 0) {
        fputs("ERROR: no pages in input raster\n", stderr);
        goto fail;
    }

    /* Media and duplex come from the page header first, and only then from the
     * option string.  The header is what the RIP actually produced, so it
     * already reflects PPD defaults that never appear in argv[5] - a queue
     * whose default is two-sided would otherwise print single-sided.  (Tumble
     * needs no handling here: it rides along in the PWG header, which is
     * copied through untouched, exactly as Canon's own driver leaves it.) */
    val       = cupsGetOption("PageSize", num_options, options);
    if (!val && *first.cupsPageSizeName)
        val = first.cupsPageSizeName;
    media     = lookup(MEDIA, val, "iso_a4_210x297mm");

    val       = cupsGetOption("MediaType", num_options, options);
    if (!val && *first.MediaType)
        val = first.MediaType;
    papertype = lookup(MEDIATYPE, val, "stationery");

    val    = cupsGetOption("Duplex", num_options, options);
    if (!val) val = cupsGetOption("sides", num_options, options);
    duplex = (val && strncasecmp(val, "one", 3) && strcasecmp(val, "None"))
             || first.Duplex;

    /* Prefer the job's real UUID; fall back to something stable if CUPS did
     * not supply one, so job_description is never empty. */
    if ((real_uuid = ivec_job_uuid(num_options, options)))
        snprintf(uuid, sizeof(uuid), "%s", real_uuid);
    else
        snprintf(uuid, sizeof(uuid), "%s-%s", job_id, user ? user : "cups");

    ivec_jobid(padded_id, sizeof(padded_id), job_id);
    job_id = padded_id;

    start_job(stdout, job_id, user, title, uuid);
    set_job_configuration(stdout, job_id);
    set_configuration(stdout, job_id, media, papertype, colormode, duplex);
    job_open = 1;

    for (i = 0; i < npages; i ++) {
        char   buf[65536];
        long   remaining = pages[i].size;

        /* nextpage stays ON until the final page, matching Canon. */
        set_page_configuration(stdout, job_id, i < npages - 1);
        send_data(stdout, job_id, pages[i].size);

        if (lseek(spool, pages[i].off, SEEK_SET) < 0) {
            fprintf(stderr, "ERROR: cannot seek spool: %s\n", strerror(errno));
            goto fail;
        }

        while (remaining > 0) {
            ssize_t n = read(spool, buf,
                             remaining < (long)sizeof(buf)
                                 ? (size_t)remaining : sizeof(buf));

            if (n <= 0) {
                fprintf(stderr, "ERROR: spool read failed with %ld bytes of "
                                "page %d left: %s\n",
                        remaining, i + 1, n < 0 ? strerror(errno) : "short file");
                goto fail;
            }

            if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n) {
                fputs("ERROR: short write sending raster\n", stderr);
                goto fail;
            }

            remaining -= n;
        }
    }

    end_job(stdout, job_id);
    job_open = 0;
    fflush(stdout);

    close(spool);
    free(pages);
    cupsFreeOptions(num_options, options);
    return 0;

fail:
    /* SendData has already promised the printer an exact byte count.  Bailing
     * out silently leaves it waiting mid-job for data that will never arrive,
     * so close the job even though it is incomplete - if stdout is itself the
     * thing that broke, this write simply fails too and costs nothing. */
    if (job_open) {
        end_job(stdout, job_id);
        fflush(stdout);
    }

    if (spool >= 0)
        close(spool);
    free(pages);
    cupsFreeOptions(num_options, options);
    return 1;
}
