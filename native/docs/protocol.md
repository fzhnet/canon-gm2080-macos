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
