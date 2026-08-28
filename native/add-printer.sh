#!/usr/bin/env bash
#
# Creates the macOS print queue for a Canon GM2080 after the driver package
# has been installed.
#
#     ./add-printer.sh 192.168.1.50 [queue-name]
set -euo pipefail

PRINTER_IP="${1:-}"
QUEUE="${2:-Canon_GM2080}"
PPD="/Library/Printers/PPDs/Contents/Resources/canongm2080-native.ppd"
FILTER="/Library/Printers/canon-gm2080/rastertocanonijgm"

die() { printf '\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

[[ -n "$PRINTER_IP" ]] || die "usage: $0 <printer-ip> [queue-name]"
[[ -f "$PPD"    ]] || die "driver not installed: $PPD is missing. Run the .pkg first."
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
