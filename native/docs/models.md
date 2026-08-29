# Which models this driver covers

Canon sells the same mono MegaTank hardware under different model numbers per
region. The driver does not care about the retail name — it cares about the
*series*, which is what the printer reports over SNMP and what Canon's own
PPDs are keyed to.

## The four series

| Driver series | Retail model | Region | Black ink | Optional colour | Scanner / ADF |
|---|---|---|---|---|---|
| GM2000 | GM2070 | India, South & SE Asia | GI-70 | CL-741 | no |
| **GM2080** | **GM2080** | **China** | **GI-80PGBK** | **CL-841** | no |
| GM4000 | GM4070 | India, South & SE Asia | GI-70 | CL-741 | yes |
| GM4080 | GM4080 | China | GI-80PGBK | CL-841 | yes |

The 4-series adds a flatbed scanner and a 35-sheet ADF. That is scanner
hardware; **the print path is identical**, and this is a print driver, so it
makes no difference here. Scanning is out of scope — use Canon's IJ Scan
Utility or any SANE/ICA front end.

The ink and cartridge part numbers differ only by region. They are the same
formulation in different boxes; nothing in the driver depends on them.

## Why one driver covers all four

Not an assumption — measured. Diffing the capability options across Canon's
own `canongm2000.ppd`, `canongm2080.ppd`, `canongm4000.ppd` and
`canongm4080.ppd` from cnijfilter2 6.81:

```
$ diff <(grep -E '^\*(PageSize|InputSlot|Duplex|MediaType|Resolution|OpenUI|CNIJ)' canongm2080.ppd | sort -u) \
       <(grep -E '^\*(PageSize|InputSlot|Duplex|MediaType|Resolution|OpenUI|CNIJ)' canongm4080.ppd | sort -u)
$ echo $?
0
```

Zero differences. All four take the same paper sizes, the same input slots,
the same media types, 600 dpi, and auto duplex.

GM2000 and GM2080 differ by 22 lines in total, and every one of them is an
identity string, an encoded blob, or a build timestamp:

```
< *ModelName: "Canon GM2000 series"        > *ModelName: "Canon GM2080 series"
< *Product: "(gm2000)"                     > *Product: "(gm2080)"
< *% internalversion : 5.90.20190702...    > *% internalversion : 5.90.2019...
```

That is why `ppd/gm-series.ppd.in` is one template expanded four times rather
than four hand-maintained files.

## What is actually tested

Be clear about this before trusting the table above.

| Series | Protocol verified | Hardware |
|---|---|---|
| GM2080 | yes — byte-compared against Canon's driver output | the development unit |
| GM2000 | no | none available |
| GM4000 | no | none available |
| GM4080 | no | none available |

The GM2080 unit reports:

```
MFG:Canon;CMD:BJRaster3,NCCe,IVEC;MDL:GM2080 series;CID:CA_IVEC1TYPE2_IJP;
```

The other three are expected to report the same `CMD:` and `CID:` values, since
Canon drives them through the same filter chain — but that expectation comes
from reading Canon's PPDs, not from a device. **If you have one, please open an
issue with the output of:**

```bash
/usr/libexec/cups/backend/snmp <printer-ip>
```

That one line is enough to confirm or refute the whole table.

## Japan: GM2030 / GM4030

Canon Japan sells what appears to be the same hardware as **GM2030** and
**GM4030** under the GIGA TANK brand. These are *not* in cnijfilter2's PPD set,
so there is no PPD evidence for them either way, and this driver ships no PPD
named for them. If a GM2030 reports `MDL:GM2000 series`, the GM2000 PPD should
work:

```bash
./add-printer.sh <printer-ip> <queue-name> gm2000
```

Untested. Reports welcome.

## Choosing a series at setup

The installer lays down all four PPDs. `add-printer.sh` defaults to GM2080:

```bash
./add-printer.sh 192.168.1.50                        # GM2080, default queue name
./add-printer.sh 192.168.1.50 Office_GM4070 gm4000    # GM4070, for example
```

The queue name and the series are separate arguments because they are
unrelated choices; the series only selects which PPD the queue is built from.
Picking the "wrong" one of the four will still print correctly, since the
capabilities are identical — it only changes the name macOS displays.

## Sources

- Canon South & Southeast Asia, [GM2000 series driver package](https://asia.canon/en/support/0101025403) — states "Supported model: PIXMA GM2070"
- Canon India, [PIXMA GM4070 specifications](https://in.canon/en/consumer/pixma-gm4070/main/specification) — GI-70 ink, CL-741 optional colour, flatbed + 35-sheet ADF, 600×1200 dpi, auto duplex
- Canon China, [GM2080](https://www.canon.com.cn/product/gm2080/) and [GM4080](https://www.canon.com.cn/product/gm4080/index.html) — GI-80PGBK ink, CL-841 optional colour
- Canon Japan, [GIGA TANK business inkjet lineup](https://personal.canon.jp/product/printer/maxify/branding/biz-ij/large) — GM2030 / GM4030
- PPD evidence: cnijfilter2 6.81, `/usr/share/ppd/canongm*.ppd`
