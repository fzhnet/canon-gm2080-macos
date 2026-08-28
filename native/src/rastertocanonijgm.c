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
              "<ivec:bidi>0</ivec:bidi>"
              "<ivec:forcepmdetection>OFF</ivec:forcepmdetection>",
         IVEC_HEAD, job_id);
    ivec_element(out, "jobname", title);
    ivec_element(out, "username", user);
    ivec_element(out, "computername", NULL);
    ivec_emit(out, "<ivec:job_description><![CDATA[%s]]></ivec:job_description>"
              "<ivec:host_environment>linux</ivec:host_environment>"
              "</ivec:param_set></ivec:contents></cmd>", uuid);
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
         IVEC_HEAD, job_id, more_pages ? "ON" : "OFF");
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

typedef struct {
    int  fd;
    long size;
} page_buf_t;

int
main(int argc, char *argv[])
{
    int            num_options;
    cups_option_t  *options;
    const char     *job_id, *user, *title, *val;
    const char     *media, *papertype, *colormode;
    int            duplex, infd, i;
    cups_raster_t  *ras_in;
    cups_page_header2_t header;
    page_buf_t     *pages = NULL;
    int            npages = 0, cap = 0;
    char           uuid[64];

    if (argc < 6 || argc > 7) {
        fputs("ERROR: rastertocanonijgm job user title copies options [file]\n",
              stderr);
        return 1;
    }

    job_id = argv[1];
    user   = argv[2];
    title  = argv[3];

    num_options = cupsParseOptions(argv[5], 0, &options);

    val       = cupsGetOption("PageSize", num_options, options);
    media     = lookup(MEDIA, val, "iso_a4_210x297mm");

    val       = cupsGetOption("MediaType", num_options, options);
    papertype = lookup(MEDIATYPE, val, "stationery");

    /* The GM series is a single-black machine; colour is never meaningful. */
    colormode = "monochrome";

    val    = cupsGetOption("Duplex", num_options, options);
    if (!val) val = cupsGetOption("sides", num_options, options);
    duplex = (val && strncasecmp(val, "one", 3) && strcasecmp(val, "None"));

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
        char          tmpl[] = "/private/tmp/gmpwg.XXXXXX";
        int           fd;
        cups_raster_t *ras_out;
        unsigned char *line;
        unsigned      y;
        struct stat   st;

        if ((fd = mkstemp(tmpl)) < 0) {
            fprintf(stderr, "ERROR: cannot create temp file: %s\n",
                    strerror(errno));
            goto fail;
        }
        unlink(tmpl);

        if (!(ras_out = cupsRasterOpen(fd, CUPS_RASTER_WRITE_PWG))) {
            fputs("ERROR: cannot open PWG raster writer\n", stderr);
            close(fd);
            goto fail;
        }

        if (!(line = malloc(header.cupsBytesPerLine))) {
            fputs("ERROR: out of memory for raster line\n", stderr);
            cupsRasterClose(ras_out);
            close(fd);
            goto fail;
        }

        if (!cupsRasterWriteHeader2(ras_out, &header)) {
            fputs("ERROR: cannot write PWG page header\n", stderr);
            free(line);
            cupsRasterClose(ras_out);
            close(fd);
            goto fail;
        }

        /* Pass the page through unchanged: the incoming raster already has the
         * geometry the printer expects, so this is a container change only. */
        for (y = 0; y < header.cupsHeight; y ++) {
            if (cupsRasterReadPixels(ras_in, line, header.cupsBytesPerLine)
                    < header.cupsBytesPerLine)
                break;
            if (cupsRasterWritePixels(ras_out, line, header.cupsBytesPerLine)
                    < header.cupsBytesPerLine)
                break;
        }

        free(line);
        cupsRasterClose(ras_out);

        if (fstat(fd, &st) < 0 || st.st_size == 0) {
            fputs("ERROR: page produced no raster data\n", stderr);
            close(fd);
            goto fail;
        }

        if (npages >= cap) {
            page_buf_t *grown;
            cap = cap ? cap * 2 : 8;
            if (!(grown = realloc(pages, (size_t)cap * sizeof(*pages)))) {
                fputs("ERROR: out of memory tracking pages\n", stderr);
                close(fd);
                goto fail;
            }
            pages = grown;
        }

        pages[npages].fd   = fd;
        pages[npages].size = (long)st.st_size;
        npages ++;

        fprintf(stderr, "PAGE: %d 1\n", npages);
    }

    cupsRasterClose(ras_in);

    if (npages == 0) {
        fputs("ERROR: no pages in input raster\n", stderr);
        goto fail;
    }

    snprintf(uuid, sizeof(uuid), "%s-%s", job_id, user ? user : "cups");

    start_job(stdout, job_id, user, title, uuid);
    set_job_configuration(stdout, job_id);
    set_configuration(stdout, job_id, media, papertype, colormode, duplex);

    for (i = 0; i < npages; i ++) {
        char   buf[65536];
        ssize_t n;
        long   remaining = pages[i].size;

        /* nextpage stays ON until the final page, matching Canon. */
        set_page_configuration(stdout, job_id, i < npages - 1);
        send_data(stdout, job_id, pages[i].size);
        fflush(stdout);

        lseek(pages[i].fd, 0, SEEK_SET);
        while (remaining > 0 &&
               (n = read(pages[i].fd, buf,
                         remaining < (long)sizeof(buf)
                             ? (size_t)remaining : sizeof(buf))) > 0) {
            if (fwrite(buf, 1, (size_t)n, stdout) != (size_t)n) {
                fputs("ERROR: short write sending raster\n", stderr);
                goto fail;
            }
            remaining -= n;
        }
        close(pages[i].fd);

        if (remaining != 0) {
            fputs("ERROR: truncated page data\n", stderr);
            goto fail;
        }
    }

    end_job(stdout, job_id);
    fflush(stdout);

    free(pages);
    cupsFreeOptions(num_options, options);
    return 0;

fail:
    for (i = 0; i < npages; i ++)
        close(pages[i].fd);
    free(pages);
    cupsFreeOptions(num_options, options);
    return 1;
}
