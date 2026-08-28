# Canon GM2080 print bridge for macOS
#
# Canon ships no macOS driver for the GM2000/GM2080 series, and the printer
# speaks no driverless protocol (no IPP, no PWG Raster, no PDF).  It only
# understands Canon's proprietary BJRaster3/IVEC stream, which is produced by
# Canon's official Linux driver (cnijfilter2).  This image runs that driver
# inside Linux and re-exposes the printer to macOS over plain IPP.
#
# amd64 is deliberate: cnijfilter2 is closed-source and published for amd64
# only.  On Apple Silicon it runs under Rosetta via Docker Desktop.
FROM --platform=linux/amd64 ubuntu:24.04

ENV DEBIAN_FRONTEND=noninteractive

RUN apt-get update -qq \
 && apt-get install -y --no-install-recommends \
      software-properties-common ca-certificates gnupg \
 && add-apt-repository -y ppa:thierry-f/fork-michael-gruz \
 && apt-get update -qq \
 && apt-get install -y --no-install-recommends \
      cups \
      cups-client \
      cups-filters \
      cnijfilter2 \
      netcat-openbsd \
      binutils \
 && apt-get purge -y software-properties-common gnupg \
 && apt-get autoremove -y \
 && rm -rf /var/lib/apt/lists/*

COPY cupsd.conf /etc/cups/cupsd.conf
COPY entrypoint.sh /usr/local/bin/entrypoint.sh
RUN chmod +x /usr/local/bin/entrypoint.sh

EXPOSE 631

HEALTHCHECK --interval=30s --timeout=5s --start-period=20s --retries=3 \
  CMD lpstat -r >/dev/null 2>&1 || exit 1

ENTRYPOINT ["/usr/local/bin/entrypoint.sh"]
