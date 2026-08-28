# GM2080 on macOS — what was tried, what the evidence was

Recorded 2026-08-23. Target: Canon GM2080 series, firmware 1.050, reached
over the network. Host: macOS 26.5, Apple Silicon.

Addresses below are written as `192.168.1.50`; substitute your own.

The point of this document is to make the dead ends **stay** dead. Each one
below looks reasonable from the outside; each is refuted by a specific
observation, quoted here so it does not have to be rediscovered.

## The decisive fact

SNMP (`/usr/libexec/cups/backend/snmp 192.168.1.50`) returns:

```
MFG:Canon;CMD:BJRaster3,NCCe,IVEC;SOJ:CHMP,CHMPu;MDL:GM2080 series;
CLS:PRINTER;DES:Canon GM2080 series;VER:1.050;STA:10;PSE:<redacted>;
CID:CA_IVEC1TYPE2_IJP;
```

(`PSE:` is redacted here: on this model family it doubles as the printer's
admin password, and it is readable by anyone on the same network over
unauthenticated SNMP.)

`CMD:` is the printer's complete list of parseable data streams. It contains
no PDF, no PostScript, no PCL, no PWG Raster, no URF. Everything else follows
from this.

Port scan: 631 **closed**, 9100 open, 515 open, 80/443 open.

## Dead end A — AirPrint / IPP Everywhere

**Refuted by three independent observations:**

1. `CMD:` contains no format any driverless client can emit.
2. TCP 631 is closed.
3. The printer's own web UI (`http://192.168.1.50/JS_MDL/model.js`) declares
   `g_ipp_over_usb = 0`, so USB is not a way around it either.

Canon's own compatibility table lists PIXMA GM2070 and GM4070 as
`Not Supported` on macOS 14, 15 and 26 — for both driver and AirPrint. GM2080
is not listed at all.

## Dead end D — reuse Canon's macOS IJ driver framework

This is the most seductive one, because the framework is often already
installed (`/Library/Printers/Canon/BJPrinter/`) and it genuinely does speak
the right protocol. `strings` on
`Filters/Raster2CanonIJ/Raster2CanonIJ2S.bundle/Contents/MacOS/Raster2CanonIJ2S`
shows a `CCmdBufIvec` class and the full IVEC command set — `StartJob`,
`EndJob`, `GetCapability`, `GetStatus`, `ModeShift`, `VendorCmd` — as literal
XML templates. The binary is universal (`x86_64 arm64`), version 29.1.0.

**It still cannot work, for two reasons:**

1. **The model table does not exist.** Canon's architecture puts per-model
   data in `Resources/Database/CIJ<model>.db`, a bundle of opaque binary
   tables (`cnb_<id>.tbl`) selected by `*CNIJTableID` in the PPD. Canon never
   built one for the GM series — the GM2000 series ships a Windows package
   only (`win-gm2000-1_3-n_mcd.exe`), with no macOS counterpart in any region.

2. **No other model's table can substitute.** The printer's web UI declares
   `inkCOL = ['InkBlk', 'InkClr']` — a single-black, two-tank machine. Every
   candidate donor with a macOS driver (G2000/G3000 series and friends) is
   four-colour CMYK. The `.db` encodes head configuration, ink limits,
   separation LUTs and dither tables; feeding a CMYK table to a single-black
   engine is a structural mismatch, not a tuning problem.

So writing a PPD that points at Canon's macOS filter — a fix that circulates
online — fails for the GM series specifically, and will keep failing.

## Dead end (deferred) C — write a BJRaster3 driver from scratch

Not attempted. Worth recording that it is **less hopeless than it first
appears**, in case the container approach ever becomes unacceptable:

- GM2080 is monochrome. A mono BJRaster3 stream is one K plane with run-length
  compression — dramatically simpler than Canon's colour path.
- The IVEC job envelope is already in hand, extracted verbatim from
  `Raster2CanonIJ2S`.

What is still missing is the `ESC (` parameter-block encoding (media, size,
resolution, model id) and the raster framing details. Gutenprint does **not**
help: its Canon model table stops at the iP/iX/MP generation with no G or GM
models at all, and the project is discontinued upstream (Homebrew disabled its
cask on 2025-10-14). Getting the rest realistically needs a USB or network
capture of the Windows driver.

## What was built instead — B

Canon's Linux driver `cnijfilter2` 6.81 does support this printer, and ships a
model-exact PPD: `/usr/share/ppd/canongm2080.ppd`, `Canon GM2080 series
Ver.5.90`, filter chain `rastertocanonij` + `cmdtocanonij3`, 600 dpi, A4.
The package is **amd64 only**, so on Apple Silicon it runs under Rosetta.

Verifications performed:

- **Container reaches the printer.** A `linux/amd64` container connected to
  the printer on both 9100 and 80, including across a VPN link.
- **The filter chain produces a real Canon stream.** With the queue pointed at
  a `file://` device, a text job yielded 40,573 bytes beginning with
  `<?xml version="1.0" encoding="utf-8" ?><cmd xmlns:ivec="http://www.canon.com/ns/cmd/2008/07/common/"…`
  — the same IVEC envelope the printer advertises, and the same one found in
  Canon's macOS binary.
- **macOS sees a usable driverless queue.** `Get-Printer-Attributes` against
  the bridge returns `printer-state = idle`,
  `print-color-mode-default = monochrome`, `printer-resolution-default =
  600dpi`, and a `document-format-supported` list including `application/pdf`,
  `image/urf` and `image/pwg-raster`.

The one link not verified by these steps is the final socket write to the
printer, which cannot be tested without consuming paper.

## Gotchas hit while building (all fixed)

- `cupsd.conf` has **no line-continuation syntax**. A `\` at the end of a
  `<Limit>` operation list is a syntax error and the scheduler refuses to
  start.
- `lpstat -r` **exits 0 even when the scheduler is down**, printing
  "scheduler is not running". Readiness checks must grep the output; using the
  exit status hides config errors behind a later, useless
  `lpadmin: Unable to connect to server: Bad file descriptor`.
- Replacing `cupsd.conf` wholesale drops Ubuntu's `Listen /run/cups/cups.sock`,
  which is what local `lp*` clients use by default.
- `FileDevice Yes` belongs in `cups-files.conf`, not `cupsd.conf`.
- cupsd drops privileges to `lp`; a `file://` target must be writable by it.
- A launchd agent **cannot execute anything under `~/Documents`**: it is
  spawned without TCC rights to that folder and dies with exit 126,
  `Operation not permitted`. The login launcher therefore lives in
  `~/Library/Application Support/` and only calls `docker start`, so it never
  needs to read the compose file either.
- `lpadmin -m everywhere` builds its PPD from a full Get-Printer-Attributes
  response, which the bridge starts answering slightly *after* it starts
  serving its web page. Gating the install on an HTTP 200 makes queue creation
  race and fail with "cannot query printer"; gate it on `ipptool` instead.
