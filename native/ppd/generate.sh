#!/usr/bin/env bash
#
# Expands gm-series.ppd.in into one PPD per GM series.
#
# All four share a single template because Canon's own PPDs for them declare
# identical capabilities - the only differences are the model name strings.
# Only GM2080 has been tested against real hardware; see docs/models.md.
set -euo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
OUT="${1:-$HERE}"

SERIES=(GM2000 GM2080 GM4000 GM4080)

for s in "${SERIES[@]}"; do
    lower="$(echo "$s" | tr '[:upper:]' '[:lower:]')"
    sed "s/@SERIES@/${s}/g" "${HERE}/gm-series.ppd.in" \
        > "${OUT}/canon${lower}-native.ppd"
done

printf '%s\n' "${SERIES[@]}"
