# Canon GM2080 — native macOS driver

[English](README.md) · [简体中文](README.zh-CN.md)

A real CUPS driver: two small filters and a PPD, installed by a `.pkg`.
No Docker, no Rosetta, no Canon binaries. Universal (arm64 + x86_64).

Canon ships no macOS driver for the GM2000/GM2080 series and the printer
speaks no driverless protocol, so this implements Canon's own wire format
directly. See [How it works](#how-it-works).

## Supported models

Canon sells this hardware under different numbers per region. One driver
covers all four series — their capabilities are identical.

| Series | Retail model | Region | Scanner |
|---|---|---|---|
| GM2000 | GM2070 | India, South & SE Asia | no |
| **GM2080** | **GM2080** | **China** | no |
| GM4000 | GM4070 | India, South & SE Asia | yes |
| GM4080 | GM4080 | China | yes |

Only GM2080 has been verified against hardware. Japan's GM2030 / GM4030 may
also work but are undocumented by Canon's Linux PPDs. Details, evidence and
how to report a model: [docs/models.md](docs/models.md).

## Install

```bash
sudo installer -pkg dist/CanonGM2080Native-1.0.pkg -target /
./add-printer.sh 192.168.1.50                        # your printer's address
./add-printer.sh 192.168.1.50 Office_Mono             # ...with a queue name
./add-printer.sh 192.168.1.50 Office_GM4070 gm4000    # ...and another series
```

The package is unsigned, so double-clicking it is blocked by Gatekeeper; the
`installer` command above is the intended path. To produce a signed and
notarized package instead, see [Building](#building).

Installs two filters and one PPD per series:

```
/Library/Printers/canon-gm2080/rastertocanonijgm     page rendering
/Library/Printers/canon-gm2080/cmdtocanonijgm        maintenance
/Library/Printers/PPDs/Contents/Resources/canongm{2000,2080,4000,4080}-native.ppd
```

## Maintenance

```bash
./maintenance.sh nozzle       # print a nozzle check pattern
./maintenance.sh clean        # clean the print head
./maintenance.sh deepclean    # deep clean (much more ink; asks first)
./maintenance.sh systemclean  # system clean (a lot of ink; unverified)
./maintenance.sh align        # auto print head alignment
```

The printer's web UI has three more — roller cleaning, platen cleaning and
printing the alignment values. They are not here because their wire format is
not IVEC XML and could not be established from any Canon binary or capture;
use the web UI for those. `systemclean` is implemented but its type value is
inferred rather than captured, so it asks you to type a confirmation.
[docs/protocol.md](docs/protocol.md) has the evidence for each.

Add a queue name as a second argument if it is not `Canon_GM2080`.

These are CUPS command jobs, so they also work without the wrapper script:

```bash
printf '#CUPS-COMMAND\nClean all\n' > /tmp/c
lp -d Canon_GM2080 -o document-format=application/vnd.cups-command /tmp/c
```

Start with `nozzle`. If the pattern has gaps, run `clean`, then `nozzle`
again. Only reach for `deepclean` if a normal clean did not fix it — it
consumes a lot of ink from a tank you refill by hand.

## Ink levels

```bash
./ink-level.sh 192.168.1.50
```

```
  Canon Black Ink Tank     [###                     ]  14%  LOW - refill soon
  Fixed Ink Absorber 1     [###################     ]  81%
  Fixed Ink Absorber 2     [#################       ]  71%
```

The printer answers the standard Printer MIB over SNMP, so this reads the
levels straight from the device. The absorbers are service parts that fill up
as the printer cleans itself; only the ink tank is refillable.

### Why Printers & Scanners shows "no information"

CUPS refreshes its own supply figures only while a job is running: the socket
backend queries SNMP as it prints and reports the result back. Until the queue
has printed at least once there is nothing cached, and the Supply Levels tab is
empty. Print anything and it populates.

If you are still using the container bridge, the panel can stay empty even
after printing, because macOS is then talking to CUPS inside the container
rather than to the printer. The native driver talks to the printer directly and
does not have that problem.

## Building

```bash
./build.sh
```

Signed and notarized (needs a paid Apple Developer account):

```bash
xcrun notarytool store-credentials canon-gm2080 \
    --apple-id you@example.com --team-id TEAMID     # once, interactively

SIGN_APP="Developer ID Application: Your Name (TEAMID)" \
SIGN_PKG="Developer ID Installer: Your Name (TEAMID)" \
NOTARY_PROFILE=canon-gm2080 \
./build.sh
```

`build.sh` checks the identities and notary credentials before compiling,
signs both filters with a hardened runtime and secure timestamp, submits the
package, fetches Apple's log if notarization is rejected, staples the ticket,
and verifies the result with `spctl`.

No credential is ever passed to `build.sh`. `store-credentials` prompts for an
app-specific password and stores it in your keychain; the build only names the
keychain profile.

## How it works

The printer's IEEE-1284 device ID is:

```
MFG:Canon;CMD:BJRaster3,NCCe,IVEC;SOJ:CHMP,CHMPu;MDL:GM2080 series;
CID:CA_IVEC1TYPE2_IJP;
```

`CMD:` is the complete list of formats it can parse — no PDF, no PostScript,
no PCL, no PWG Raster, no URF. Port 631 is closed and the printer's own web UI
reports `g_ipp_over_usb = 0`, so there is no IPP anywhere.

What the printer actually wants turns out to be simple: **PWG Raster wrapped in
plain-text IVEC XML commands**, sent to port 9100.

```
<?xml …><cmd …><ivec:contents><ivec:operation>StartJob</ivec:operation>…
SetJobConfiguration
SetConfiguration           media, paper type, colour mode, duplex
  VendorCmd  nextpage=ON   ─┐
  SendData   datasize=N     ├─ one pair per page, ON until the last
  <PWG raster stream>      ─┘
EndJob
```

No binary framing, no length prefixes, no checksums, no escapes — which is why
this needs no Canon code. Maintenance uses the same envelope with
`servicetype="maintenance"` and a `Cleaning` or `TestPrint` operation instead
of raster data.

The format was established by differential analysis against Canon's own Linux
driver running in a container: feed it known input, capture the bytes it
produces, reproduce them. `docs/` in the parent directory records the evidence.

### Verified

- **It prints.** Confirmed on a GM2080 (firmware 1.050), macOS 26.5, Apple
  Silicon: installed from the package, added with `add-printer.sh`, and printed
  through a real CUPS queue — which also exercises the filter under `cupsd`'s
  sandbox and the `TMPDIR` it exports.
- Raster geometry matches Canon exactly: 4800 × 6826 at 600 dpi, 14400
  bytes/line, 8 bpc, 24 bpp, sRGB — the imageable area, not the full sheet.
- Multi-page structure matches: one `VendorCmd`+`SendData` per page,
  `nextpage` ON/ON/OFF, one complete PWG stream per page, and every declared
  `datasize` equal to its actual payload.
- Maintenance blocks match Canon's operation names, `servicetype`, parameter
  values, and field order.
- Namespace prefixes and per-block namespace declarations match Canon on all
  12 captured blocks. Every remaining byte of difference is accounted for and
  deliberate — see [docs/protocol.md](docs/protocol.md).
- Job title and user name are XML-escaped, so a document called
  `P&L <draft>.pdf` still produces well-formed blocks.

### Still open

- Only GM2080 has been run against hardware. The other three series use PPDs
  generated from the same template and should behave identically, but that is
  unconfirmed — see [docs/models.md](docs/models.md).
- `systemclean` is the one maintenance command whose `type` value is inferred
  rather than captured; the other four were read back from Canon's driver.
  [docs/protocol.md](docs/protocol.md) has the reasoning.

## Licence

MIT. Contains no Canon code. Canon, PIXMA and IVEC are trademarks of Canon Inc.
This is not a Canon product and is not endorsed by Canon.
