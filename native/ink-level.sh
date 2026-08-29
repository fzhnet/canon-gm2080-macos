#!/usr/bin/env bash
#
# Reads ink and waste-absorber levels straight from the printer over SNMP.
#
#     ./ink-level.sh [printer-ip]
#
# CUPS only refreshes its own marker-* values while a job runs, so the
# Printers & Scanners panel shows nothing until you have printed at least
# once.  This asks the printer directly, so it works any time.
set -euo pipefail

PRINTER_IP="${1:-}"

if [[ -z "$PRINTER_IP" ]]; then
    # Fall back to whatever the installed queue points at.
    PRINTER_IP="$(lpstat -v Canon_GM2080 2>/dev/null |
                  sed -n 's|.*socket://\([0-9.]*\).*|\1|p')"
fi

if [[ -z "$PRINTER_IP" ]]; then
    echo "Usage: $0 <printer-ip>" >&2
    exit 1
fi

command -v snmpwalk >/dev/null 2>&1 || {
    echo "ERROR: snmpwalk not found (it ships with macOS at /usr/bin/snmpwalk)" >&2
    exit 1
}

# Walk the whole prtMarkerSupplies table in one conversation rather than one
# per column: over a VPN the extra round trips are what actually fail, and a
# single dropped walk would otherwise blank the whole report.
TABLE="$(snmpwalk -v1 -c public -t 8 -r 4 -On "$PRINTER_IP" \
                  1.3.6.1.2.1.43.11.1.1 2>/dev/null || true)"

if [[ -z "$TABLE" ]]; then
    echo "ERROR: no SNMP response from ${PRINTER_IP}" >&2
    echo "       Check the address, and that the VPN is up if it is remote." >&2
    exit 1
fi

# Columns: .6 description, .8 max capacity, .9 current level, keyed by index.
parse_column() {
    echo "$TABLE" |
        sed -n "s|^\.1\.3\.6\.1\.2\.1\.43\.11\.1\.1\.$1\.1\.\([0-9]*\) = [A-Za-z0-9]*: *\(.*\)$|\1 \2|p" |
        sed 's/"//g'
}

DESCS=(); MAXES=(); LEVELS=()
while read -r idx val; do DESCS[$idx]="$val";  done < <(parse_column 6)
while read -r idx val; do MAXES[$idx]="$val";  done < <(parse_column 8)
while read -r idx val; do LEVELS[$idx]="$val"; done < <(parse_column 9)

if [[ ${#LEVELS[@]} -eq 0 ]]; then
    echo "ERROR: printer answered SNMP but reported no supplies" >&2
    exit 1
fi

printf '\nCanon GM2080 at %s\n\n' "$PRINTER_IP"

for i in "${!LEVELS[@]}"; do
    desc="${DESCS[$i]:-supply $i}"
    max="${MAXES[$i]:-100}"
    level="${LEVELS[$i]}"

    # A negative level is the Printer MIB's way of saying "unknown" or
    # "some remaining, amount not measured" - not a real percentage.
    if [[ "$level" -lt 0 ]]; then
        printf '  %-24s  %s\n' "$desc" "unknown"
        continue
    fi

    [[ "$max" -gt 0 ]] || max=100
    pct=$(( level * 100 / max ))

    filled=$(( pct * 24 / 100 ))
    bar="$(printf '%*s' "$filled" '' | tr ' ' '#')"
    bar="$bar$(printf '%*s' "$(( 24 - filled ))" '')"

    # Only the ink tank is something you act on; absorbers are service items.
    if [[ "$desc" == *"Ink Tank"* && "$pct" -lt 20 ]]; then
        printf '  %-24s [%s] %3d%%  \033[31mLOW - refill soon\033[0m\n' \
               "$desc" "$bar" "$pct"
    else
        printf '  %-24s [%s] %3d%%\n' "$desc" "$bar" "$pct"
    fi
done

printf '\n  Ink Tank is the refillable black tank.\n'
printf '  Ink Absorbers are service parts; they fill up as the printer cleans.\n\n'
