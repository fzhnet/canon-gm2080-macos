# Canon GM2080 on macOS

Canon ships **no macOS driver** for the GM2000/GM2080 series, and the printer
supports **no driverless protocol**. This project makes it work anyway, by
running Canon's official *Linux* driver inside a container and re-exposing the
printer to macOS over plain IPP.

Verified on macOS 26.5 (Apple Silicon) against a GM2080 at firmware 1.050.

```
┌─ macOS ────────────────────────────────────────────────┐
│  Print dialog                                          │
│      │ PDF over IPP                                    │
│      ▼                                                 │
│  queue "Canon_GM2080"  ──▶  127.0.0.1:6631             │
└──────────────────────────────────│─────────────────────┘
                                   ▼
┌─ Docker (linux/amd64, Rosetta) ────────────────────────┐
│  CUPS + Canon cnijfilter2 6.81                         │
│  PDF ─▶ CUPS raster ─▶ rastertocanonij                 │
│                          │ BJRaster3 / IVEC            │
└──────────────────────────│─────────────────────────────┘
                           ▼  tcp/9100  (over VPN)
                    Canon GM2080  @ $PRINTER_IP  
```

## Install

Requires Docker Desktop, running.

```bash
cp docker/.env.example docker/.env
# edit docker/.env and set PRINTER_IP to your printer's address
./scripts/install.sh
```

That builds the container, starts it, adds the macOS queue `Canon_GM2080`, and
installs a launchd agent so the bridge returns after a reboot.

Then print to **Canon_GM2080** from any app.

Make sure Docker Desktop is set to **start at login** (Settings → General).
The container carries `restart: unless-stopped`, so Docker brings it back by
itself; the launchd agent is only a backstop for when that does not fire.

## Uninstall

```bash
./scripts/uninstall.sh          # keeps the built image
./scripts/uninstall.sh --purge  # deletes it too
```

## Changing the printer's IP

Edit `PRINTER_IP` in `docker/.env`, then re-run `./scripts/install.sh`.
That file is git-ignored — it holds your local addresses, not the project's.

## Why it has to be this complicated

The GM2080 reports this IEEE-1284 device ID over SNMP:

```
MFG:Canon;CMD:BJRaster3,NCCe,IVEC;SOJ:CHMP,CHMPu;MDL:GM2080 series;
VER:1.050;CID:CA_IVEC1TYPE2_IJP;
```

`CMD:` lists everything the printer can parse. There is no PDF, no PostScript,
no PCL, no PWG Raster and no URF — only Canon's proprietary **BJRaster3**,
wrapped in Canon's **IVEC** XML command protocol. Port 631 is closed and the
printer's own web UI reports `g_ipp_over_usb = 0`, so there is no IPP on either
the network or the USB side.

That rules out every driverless path. Producing BJRaster3 requires Canon's
closed-source rasteriser, and the only build of it that exists for a
Unix-like OS is the Linux `cnijfilter2` package — hence the container.

Two approaches that look plausible but are **dead ends** — see
[`docs/2026-08-23-investigation.md`](docs/2026-08-23-investigation.md) for the
full evidence, so nobody burns a day rediscovering this:

- **AirPrint / IPP Everywhere** — the printer speaks no IPP at all.
- **Grafting a PPD onto Canon's macOS IJ driver framework** — the framework is
  installed on many Macs and does implement IVEC, but it needs a per-model
  binary table (`CIJ<model>.db`) that Canon never built for the GM series. No
  other model's table can substitute: GM2080 is a single-black, two-tank
  machine, and every donor model with a macOS driver is four-colour CMYK.

## Troubleshooting

**Nothing prints, jobs sit in the queue.**
Check that the printer is powered on and reachable — including the VPN, if
yours sits behind one.

```bash
nc -z -G 5 "$(grep PRINTER_IP docker/.env | cut -d= -f2)" 9100 && echo reachable
```

**Is the bridge alive?**

```bash
docker ps --filter name=canon-gm2080-bridge
docker compose -f docker/docker-compose.yml logs -f
```

**Did the job reach the bridge?**

```bash
curl -s http://127.0.0.1:6631/printers/GM2080 -o /dev/null -w '%{http_code}\n'
```

**Bridge did not start after a reboot.**
Docker Desktop must be running and set to start at login. The agent waits up
to 10 minutes for it:

```bash
cat ~/Library/Logs/com.local.canon-gm2080-bridge.log
```

**Start it by hand:**

```bash
./scripts/start-bridge.sh
```

**`Operation not permitted` in the agent log.**
The launchd agent must never be pointed at a script inside `~/Documents`:
launchd spawns it without TCC rights to that folder and it dies with exit 126.
`install.sh` therefore installs its launcher to
`~/Library/Application Support/canon-gm2080-bridge/`, which is not
TCC-protected, and that launcher only calls `docker start` so it never needs
to read anything from this project directory. If you move the project, re-run
`./scripts/install.sh`.

## Security notes

- The container's CUPS has permissive access rules, but its port is published
  **only on 127.0.0.1**. Never change the port mapping in
  `docker-compose.yml` to `0.0.0.0` — that would expose an unauthenticated
  print server to your whole network.
- The printer's admin password is also its `PSE:` field, readable by anyone on
  the network via unauthenticated SNMP. Consider changing it in the printer's
  web UI.

## What this project does and does not ship

Everything here is glue: a Dockerfile, a CUPS configuration and some shell
scripts, all MIT-licensed (see [LICENSE](LICENSE)).

It contains **no Canon code**. The driver itself — `cnijfilter2`, which is
closed source and redistributed by Canon under its own licence — is fetched at
image build time from the [Ordissimo
PPA](https://launchpad.net/~thierry-f/+archive/ubuntu/fork-michael-gruz), which
packages Canon's official Linux releases. Nothing proprietary is vendored into
this repository, and building the image means accepting Canon's licence terms
for that package.

## Layout

```
docker/
  Dockerfile           Ubuntu 24.04 + CUPS + cnijfilter2 (amd64, Rosetta)
  cupsd.conf           bridge CUPS config — no line continuations allowed
  docker-compose.yml   printer IP and port mapping live here
  entrypoint.sh        starts cupsd, creates the queue
scripts/
  install.sh           build + start + macOS queue + launchd agent
  uninstall.sh         removes all of the above
  start-bridge.sh      manual start: waits for Docker, runs `compose up -d`

Installed outside the project by install.sh:
  ~/Library/Application Support/canon-gm2080-bridge/start-bridge.sh
                       login launcher (must live outside ~/Documents; see
                       Troubleshooting)
  ~/Library/LaunchAgents/com.local.canon-gm2080-bridge.plist
docs/
  2026-08-23-investigation.md
docker/.env                git-ignored; your printer's address lives here
docker/.env.example        template, committed
```
