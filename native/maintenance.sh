#!/usr/bin/env bash
#
# Printer maintenance for the Canon GM2080 native driver.
#
#     ./maintenance.sh nozzle       print a nozzle check pattern
#     ./maintenance.sh clean        clean the print head
#     ./maintenance.sh deepclean    deep clean (uses substantially more ink)
#     ./maintenance.sh align        auto print head alignment
#
# Add a queue name as the second argument if it is not Canon_GM2080.
#
# These go to the printer as CUPS command jobs, which the cmdtocanonijgm
# filter turns into IVEC maintenance operations.
set -euo pipefail

ACTION="${1:-}"
QUEUE="${2:-Canon_GM2080}"

usage() {
    sed -n '3,12p' "${BASH_SOURCE[0]}" | sed 's/^# \{0,1\}//'
    exit 1
}

[[ -n "$ACTION" ]] || usage

case "$ACTION" in
    nozzle)    COMMAND="PrintSelfTestPage"       ; DESC="nozzle check pattern" ;;
    clean)     COMMAND="Clean all"               ; DESC="head cleaning" ;;
    deepclean) COMMAND="com.canon.deepclean"     ; DESC="deep head cleaning" ;;
    align)     COMMAND="com.canon.autoalignment" ; DESC="auto head alignment" ;;
    -h|--help) usage ;;
    *)
        printf '\033[31mUnknown action: %s\033[0m\n\n' "$ACTION" >&2
        usage
        ;;
esac

if ! lpstat -p "$QUEUE" >/dev/null 2>&1; then
    printf '\033[31mERROR: no such print queue: %s\033[0m\n' "$QUEUE" >&2
    echo "Available queues:" >&2
    lpstat -p 2>/dev/null | awk '{print "    " $2}' >&2 || true
    exit 1
fi

# Cleaning cycles consume ink from a tank the user has to refill by hand, so
# the expensive one asks first rather than firing on a typo.
if [[ "$ACTION" == "deepclean" ]]; then
    printf 'Deep cleaning uses substantially more ink than a normal clean.\n'
    printf 'Run it only if a normal clean did not fix the print quality.\n\n'
    read -r -p "Proceed? [y/N] " reply
    [[ "$reply" =~ ^[Yy]$ ]] || { echo "Cancelled."; exit 0; }
fi

CMDFILE="$(mktemp -t gmmaint)"
trap 'rm -f "$CMDFILE"' EXIT
printf '#CUPS-COMMAND\n%s\n' "$COMMAND" > "$CMDFILE"

printf 'Sending %s to %s ...\n' "$DESC" "$QUEUE"
lp -d "$QUEUE" -o document-format=application/vnd.cups-command "$CMDFILE"

case "$ACTION" in
    nozzle)
        printf '\nA test pattern will print. If lines are broken or missing,\n'
        printf 'run:  %s clean %s\n' "$0" "$QUEUE"
        ;;
    clean|deepclean)
        printf '\nThe printer will run its cleaning cycle - this takes a minute\n'
        printf 'and is noisy. Check the result with:  %s nozzle %s\n' "$0" "$QUEUE"
        ;;
    align)
        printf '\nAn alignment sheet will print and be read back automatically.\n'
        ;;
esac
