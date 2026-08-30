# IVEC command envelope, as Canon actually sends it

Everything here was obtained by running Canon's own Linux driver against a
`file://` queue and reading the bytes. Where this driver deviates from that
reference, the deviation is deliberate and explained.

Verification method: capture Canon's output for the same job, split both
streams into `<?xml …</cmd>` blocks, and compare operation names, namespace
declarations, element names *with their prefixes*, and order.

## Namespace prefixes are load-bearing

`vcn:host_environment` and `ivec:host_environment` are different elements to
an XML parser. Getting this wrong is silent: the printer sees an unknown
element, ignores it, and falls back to a default.

Print path:

| Block | Declares `vcn` | `vcn:` elements used |
|---|---|---|
| StartJob | yes | `forcepmdetection`, `host_environment` |
| SetJobConfiguration | no | — |
| SetConfiguration | no | — |
| VendorCmd | yes | `ijoperation`, `nextpage` |
| SendData | no | — |
| EndJob | no | — |

Maintenance path:

| Block | Declares `vcn` | `vcn:` elements used |
|---|---|---|
| StartJob | yes | `host_environment` |
| SetJobConfiguration | **yes** | **none** |
| Cleaning / TestPrint | no | — |
| EndJob | no | — |

Note the maintenance `SetJobConfiguration`: it declares a namespace it never
uses, while the print path's equivalent block does not declare it at all. So
"declare `vcn` iff the block uses it" is *not* the rule. There is no rule —
each emitter reproduces what Canon sends for that block.

## Job id is zero-padded

Canon always sends the job id as 8 digits — `00000002`, never `2`. Cheap to
match, and cheap insurance against width-sensitive firmware.

## job_description carries the job UUID

Canon puts a bare UUID there. CUPS supplies the real one in the filter options
as `job-uuid=urn:uuid:…`; strip the URN prefix. Falls back to
`<jobid>-<user>` when absent so the element is never empty.

## Element values must be XML-escaped

`jobname` and `username` carry a job title and a user name — whatever the
person printing typed. Canon's reference captures always had these empty, so
the wire format says nothing about escaping, but the blocks are XML and the
firmware parses them: a document called `P&L <draft>.pdf` would otherwise emit
a malformed StartJob, and a crafted title could close `jobname` early and
inject elements. `job_description` is CDATA, which needs the separate `]]>`
split. Both are handled in `ivec.c`, the only place that writes element text.

## Duplex does not travel in the raster header on macOS

Canon's driver puts two-sided printing in two places at once: `duplexprint`
ON/OFF in the `SetConfiguration` block, and the `Duplex`/`Tumble` fields of
every PWG page header. The header is where the binding edge lives — there is
no IVEC element for it, and Canon's long-edge and short-edge captures differ
by exactly one byte, at header offset 368.

Those header fields are filled in by the RIP, from the PPD's
`<</Duplex true/Tumble true>>setpagedevice` code. Ghostscript does that on
Linux. **macOS's `cgpdftoraster` does not** — measured against the real chain,
`Duplex` (offset 272) and `Tumble` (368) are zero even for an explicit
`-o Duplex=DuplexTumble`, while `cupsWidth`/`cupsHeight` in the same header
read correctly, so the offsets are not in doubt.

So a filter that reads duplex out of the incoming header works when tested
against Canon's Linux driver and silently does nothing on the platform it
ships for. This driver instead resolves the setting from the PPD
(`ppdMarkDefaults` + `cupsMarkOptions` + `ppdFindMarkedChoice`, which also
translates the IPP `sides` spellings) and stamps `Duplex`/`Tumble` into each
page header on the way out, so the header agrees with the command block.

Media is different: `cupsPageSizeName` and `MediaType` *are* populated by
cgpdftoraster, so the header remains a usable fallback for those.

## Maintenance coverage, and what is missing

The printer's own web UI offers eight maintenance functions. This driver
implements five of them.

| Web UI | IVEC | Status |
|---|---|---|
| 打印喷嘴检查图案 nozzle check | `TestPrint type=nozzle_check` | captured |
| 清洗 clean | `Cleaning type=regular inkgroup=all` | captured |
| 深度清洗 deep clean | `Cleaning type=deep inkgroup=all` | captured |
| 自动打印头对齐 auto alignment | `TestPrint type=auto_registration` | captured |
| 墨水系统冲洗 system clean | `Cleaning type=choke inkgroup=all` | **inferred** |
| 打印打印头对齐数值 | — | not implemented |
| 滚轴清洁 roller cleaning | — | not implemented |
| 底板清洁 platen cleaning | — | not implemented |

"Captured" means the exact bytes were read back from Canon's Linux driver
running against a `file://` queue. `cnijfilter2` exposes only three commands,
so the other two came from Canon's macOS utility bundles instead.

**Why `choke` is only inferred.** The full `Cleaning`/`TestPrint` type enum in
`CIJUtilityCommand2.bundle` is `regular`, `deep`, `choke`, `nozzle_check`,
`lf_adjust`, `auto`, with `inkgroup` values `all`, `group1`…`group3`, `none`.
`choke` is the only member that fits a system clean, and Canon's utility has
matching `SystemCleaningConfirmationGuide` strings ("系统清洗消耗大量墨水").
The operation and parameter shape are therefore exact; only the mapping from
the menu label to `choke` is an inference. It cannot be settled from here:
`cnijfilter2` never emits it, and the printer answers nothing on port 9100 —
a `GetCapability` sent there returns zero bytes, because 9100 is a one-way
data stream. Canon's utility gets its answers over BJNP/CHMP on UDP 8611,
which this printer does not expose.

**Why the last three are absent.** Roller and platen cleaning are not IVEC XML
at all. `CIJUtilityControl2.bundle` has `cijPrinterCommandRollerCleaning:` and
`cijPrinterCommandPlatenCleaning:` methods, but the only XML templates in that
bundle are `GetStatus`, `GetCapability` and `VendorCmd`/`ModeShift`; neither
`BJCommand2.framework` nor `BJMPILib.framework` contains a matching string.
Printing the alignment values is presumably a `TestPrint` variant, but the two
remaining enum members (`lf_adjust`, `auto`) map to no evidence either way.

Guessing here is not like guessing in software: a wrong command goes to a
physical device, and the cheapest of these operations still costs ink. All
three remain available from the printer's web UI and front panel, which is
where they should be used until someone can capture them.

## Two Canon defects this driver does not reproduce

**1. A corrupted XML declaration.** In the maintenance `SetJobConfiguration`
block only, Canon emits:

```xml
<?xml version="1.0" encoding="job_descriptionutf-8" ?>
```

`job_description` has leaked into the `encoding` attribute — a formatting bug
in `cmdtocanonij3`. It appears in exactly one of the twelve blocks captured;
the print path is clean throughout. The printer evidently tolerates it. We
emit `encoding="utf-8"`, which accounts for a 15-byte difference in that
block.

**2. Inconsistent whitespace before `?>`.** Ten of Canon's twelve blocks end
the declaration `encoding="utf-8" ?>`; two use `encoding="utf-8"?>`, and which
is which does not correlate with anything. Both are valid XML. We use the
majority form everywhere, which costs 1 byte in each of those two blocks.

Reproducing either would mean carrying per-block literals to imitate defects,
which is worse code for no behavioural gain.

## Current fidelity

Against a captured reference for the same job:

| Path | Blocks | Structurally identical | Byte-identical |
|---|---|---|---|
| Print | 8 | 8 / 8 | 6 / 8 |
| Maintenance | 4 | 4 / 4 | 2 / 4 |

Every remaining byte is accounted for:

- print `StartJob` — the reference job had empty `jobname`/`username` and a
  different UUID; format matches exactly
- print `SendData` — `datasize` differs because our rasteriser compresses to a
  slightly different length, which is the point of the field
- maintenance `SetJobConfiguration` — Canon's corrupted encoding attribute
- maintenance `Cleaning` — the whitespace inconsistency

No unexplained differences remain.

## Reproducing the comparison

```bash
# capture Canon's output for the same job
docker run --rm --platform linux/amd64 -v /private/tmp:/out \
    --entrypoint bash canon-gm2080-bridge:latest -c '...'   # see git history

# then diff block by block, comparing prefixes and not just element names
```

The trap worth remembering: an early version of this driver compared
*operation names and parameter presence* and reported a perfect match, while
two elements were in the wrong namespace. Compare prefixes.
