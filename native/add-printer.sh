#!/usr/bin/env bash
#
# Creates the macOS print queue for a Canon GM2080 after the driver package
# has been installed.
#
#     ./add-printer.sh 192.168.1.50 [queue-name]
set -euo pipefail

PRINTER_IP="${1:-}"
QUEUE="${2:-Canon_GM2080}"
# Default to the series this driver was developed and tested against.  The
# other GM series use identical PPDs; pass one as the second argument.
SERIES="${2:-gm2080}"
PPD="/Library/Printers/PPDs/Contents/Resources/canon$(echo "$SERIES" | tr '[:upper:]' '[:lower:]')-native.ppd"
FILTER="/Library/Printers/canon-gm2080/rastertocanonijgm"

die() { printf '\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

[[ -n "$PRINTER_IP" ]] || die "usage: $0 <printer-ip> [queue-name]"
if [[ ! -f "$PPD" ]]; then
    echo "No PPD for series '${SERIES}'. Installed series:" >&2
    ls /Library/Printers/PPDs/Contents/Resources/canongm*-native.ppd 2>/dev/null |
        sed 's|.*/canon\(.*\)-native.ppd|    \1|' >&2 ||
        echo "    (none - run the .pkg first)" >&2
    exit 1
fi
[[ -x "$FILTER" ]] || die "driver not installed: $FILTER is missing. Run the .pkg first."

# Port 9100 is the printer's raw port; the GM series exposes no IPP at all.
if nc -z -G 5 "$PRINTER_IP" 9100 2>/dev/null; then
    echo "    printer reachable at ${PRINTER_IP}:9100"
else
    echo "    WARNING: ${PRINTER_IP}:9100 did not answer. Adding the queue anyway."
fi

lpadmin -p "$QUEUE" \
        -v "socket://${PRINTER_IP}:9100" \
        -P "$PPD" \
        -D "Canon GM2080 series" \
        -L "$PRINTER_IP" \
        -E \
        -o printer-is-shared=false

cupsenable "$QUEUE" 2>/dev/null || true
cupsaccept "$QUEUE" 2>/dev/null || true

echo "    queue created: $(lpstat -v "$QUEUE")"
echo
echo "    Test with:  lp -d ${QUEUE} <some-file>"
