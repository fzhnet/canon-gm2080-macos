# Canon GM2080 on macOS

[English](README.md) · [简体中文](README.zh-CN.md)

Canon ships **no macOS driver** for the GM2000/GM2080 series, and the printer
supports **no driverless protocol** — no AirPrint, no IPP, no PDF, no PCL. This
repository makes it print anyway, two different ways.

Developed against a GM2080 (firmware 1.050) on macOS 26.5, Apple Silicon.

| | [`native/`](native/) | [`bridge/`](bridge/) |
|---|---|---|
| What it is | A real CUPS driver: `.pkg`, PPD, filter | Canon's Linux driver in a container, re-exposed over IPP |
| Dependencies | none | Docker Desktop |
| Architecture | arm64 + x86_64 native | amd64 under Rosetta |
| Canon code | none | Canon's official closed-source driver |
| Status | **works — confirmed printing on a GM2080** | works, but unconfirmed on paper |

The native driver has been confirmed end to end: installed from the package,
added as a queue, and printed. See [Status](#status) for what else is checked.

## If you got here from a search

If you were looking for any of these, the answer is yes:

- canon gm2080 mac driver · gm2070 macos · gm4080 apple silicon
- canon gm series no macOS driver · canon megatank mac driver
- "cannot communicate with the printer" when adding a Canon GM on macOS

That last one is almost always the protocol picker: macOS defaults to IPP,
which is the one protocol these printers do not speak. Use **HP Jetdirect —
Socket** instead and leave the queue name empty.

Canon's own macOS compatibility table lists the GM series as Not Supported for
both driver and AirPrint, which is why this project exists.

## Which one to use

Start with **`native/`**. It is a normal driver: install a package, add the
printer, done. Nothing keeps running in the background.

Fall back to **`bridge/`** if the native driver misbehaves. It routes pages
through Canon's own driver, so its output is correct by construction — the
cost is a container that has to be running whenever you print.

## native — install

Download the package from [Releases](../../releases/latest), then:

```bash
sudo installer -pkg CanonGM2080Native-1.0.1.pkg -target /
./native/add-printer.sh <printer-ip>
```

Or build it yourself — needs the Xcode command line tools
(`xcode-select --install`):

```bash
cd native && ./build.sh
sudo installer -pkg dist/CanonGM2080Native-1.0.1.pkg -target /
./add-printer.sh <printer-ip>
```

The package is unsigned, so double-clicking it is blocked by Gatekeeper; the
`installer` command above is the intended path.

When adding the printer by hand in System Settings, set **Protocol** to
**HP Jetdirect — Socket**, not IPP. IPP is the one protocol this printer does
not speak, and it is what macOS selects by default.

Installs two filters and one PPD per model series:

```
/Library/Printers/canon-gm2080/rastertocanonijgm     page rendering
/Library/Printers/canon-gm2080/cmdtocanonijgm        maintenance
/Library/Printers/PPDs/Contents/Resources/canongm{2000,2080,4000,4080}-native.ppd
```

Maintenance and ink levels:

```bash
./native/maintenance.sh nozzle          # nozzle check, clean, deepclean, align
./native/ink-level.sh <printer-ip>      # read supply levels over SNMP
```

## bridge — install

Requires Docker Desktop, running and set to start at login.

```bash
cp bridge/docker/.env.example bridge/docker/.env
# edit bridge/docker/.env and set PRINTER_IP
./bridge/scripts/install.sh
```

Remove it with `./bridge/scripts/uninstall.sh` (add `--purge` to drop the
image too).

## How the printer actually works

The GM2080 reports this over SNMP:

```
MFG:Canon;CMD:BJRaster3,NCCe,IVEC;SOJ:CHMP,CHMPu;MDL:GM2080 series;
```

`CMD:` is the complete list of data it can parse. No PDF, no PostScript, no
PCL, no PWG Raster, no URF. Port 631 is closed and the printer's own web UI
reports `g_ipp_over_usb = 0`, so there is no IPP on either transport. That is
why no amount of PPD-writing alone can help, and why AirPrint cannot work.

What it *does* accept turns out to be simple — plain-text XML commands with a
standard PWG Raster payload, concatenated with no binary framing at all:

```
StartJob                                  ┐
SetJobConfiguration                       │ IVEC XML
SetConfiguration    (media, colour, duplex)┘
  VendorCmd  nextpage=ON   ┐
  SendData   datasize=N    │ once per page
  <PWG Raster page>        ┘
  ...
  VendorCmd  nextpage=OFF     ← last page only
  SendData / <PWG Raster page>
EndJob
```

No length prefixes, no checksums, no escaping. macOS can already produce PWG
Raster, so the native driver needs no Canon code — it writes the envelope and
passes the raster through. Full details, including how each field was
determined, are in
[`docs/2026-08-28-protocol.md`](docs/2026-08-28-protocol.md).

## Status

**Verified for the native driver:**

- **The printer accepts the stream and prints.** Confirmed on a GM2080
  (firmware 1.050) from macOS 26.5 on Apple Silicon, through a real CUPS queue
  — which also exercises the filter under `cupsd`'s sandbox.
- Output is byte-structurally identical to Canon's own driver: the same
  command blocks in the same order, with the same namespace prefixes and
  per-block namespace declarations, for both single-page and multi-page jobs
  (`nextpage` ON/ON/OFF across three pages, one PWG stream per page, every
  declared `datasize` equal to its actual payload).
- Raster geometry matches Canon exactly — 4800×6826 at 600 dpi for A4,
  14400 bytes per line, 8 bits per colour, 24 bits per pixel, sRGB.
- Media and paper-type tables were read back from Canon's driver rather than
  guessed.
- Compiles warning-free as a universal binary; the PPD passes `cupstestppd`.
- Job titles and user names are XML-escaped, so a file named `P&L <draft>.pdf`
  still produces well-formed command blocks.

**Still open:** only the GM2080 has been run against hardware. The GM2000,
GM4000 and GM4080 PPDs are generated from the same template and should behave
identically, but nobody has confirmed that on a device — see
[`native/docs/models.md`](native/docs/models.md). The `bridge/` path has not
been printed through either.

If you test it, please open an issue saying what happened — success or not.

## Troubleshooting

**Jobs queue but nothing prints.** Check the printer is reachable:

```bash
nc -z -G 5 <printer-ip> 9100 && echo reachable
```

**Native driver: see what the filter is doing.**

```bash
cupsctl --debug-logging
lp -d Canon_GM2080 somefile.pdf
tail -f /var/log/cups/error_log
```

**Bridge: check the container.**

```bash
docker ps --filter name=canon-gm2080-bridge
docker compose -f bridge/docker/docker-compose.yml logs -f
```

**Bridge: `Operation not permitted` in the agent log.** A launchd agent cannot
execute anything under `~/Documents` — it is spawned without TCC rights there
and dies with exit 126. `install.sh` puts its launcher in
`~/Library/Application Support/` for this reason. If you move the project,
re-run the installer.

## What this project ships

`native/` contains no Canon code. It is an independent implementation of the
command envelope, written against observed behaviour, and is MIT-licensed
along with the rest of this repository (see [LICENSE](LICENSE)).

`bridge/` contains no Canon code either: it builds an image that installs
`cnijfilter2` — closed source, redistributed by Canon under its own licence —
from the [Ordissimo
PPA](https://launchpad.net/~thierry-f/+archive/ubuntu/fork-michael-gruz) at
build time. Nothing proprietary is vendored here, and building that image
means accepting Canon's licence for that package.

Canon, PIXMA and IVEC are trademarks of Canon Inc. This is not a Canon product
and is not affiliated with or endorsed by Canon.

## Two dead ends, documented

[`docs/2026-08-23-investigation.md`](docs/2026-08-23-investigation.md) records
two approaches that look plausible, with the evidence that kills each — so
nobody spends a day rediscovering them:

- **AirPrint / IPP Everywhere.** The printer speaks no IPP at all.
- **Grafting a PPD onto Canon's macOS IJ driver framework.** That framework is
  installed on many Macs and does implement IVEC, but it needs a per-model
  binary table Canon never built for the GM series — and no donor model's
  table fits a single-black, two-tank machine.
