#!/usr/bin/env bash
#
# Builds the native driver and wraps it in a macOS installer package.
#
# Output: dist/CanonGM2080Native-<version>.pkg
set -euo pipefail

VERSION="${VERSION:-1.0}"
IDENTIFIER="com.local.canon-gm2080-native"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${HERE}/build"
ROOT="${BUILD}/pkgroot"
DIST="${HERE}/dist"

FILTER_DIR="/Library/Printers/canon-gm2080"
PPD_DIR="/Library/Printers/PPDs/Contents/Resources"

say() { printf '\n\033[1m==> %s\033[0m\n' "$*"; }

rm -rf "$BUILD"
mkdir -p "${ROOT}${FILTER_DIR}" "${ROOT}${PPD_DIR}" "$DIST"

say "Compiling rastertocanonijgm (universal)"
clang -O2 -Wall -Wextra -Wno-deprecated-declarations \
      -arch arm64 -arch x86_64 \
      -o "${ROOT}${FILTER_DIR}/rastertocanonijgm" \
      "${HERE}/src/rastertocanonijgm.c" -lcups
lipo -info "${ROOT}${FILTER_DIR}/rastertocanonijgm"

# CUPS refuses to run a filter that is group- or world-writable.
chmod 755 "${ROOT}${FILTER_DIR}/rastertocanonijgm"

say "Staging PPD"
cp "${HERE}/canongm2080-native.ppd" "${ROOT}${PPD_DIR}/canongm2080-native.ppd"
chmod 644 "${ROOT}${PPD_DIR}/canongm2080-native.ppd"

say "Validating PPD against the staged filter path"
# cupstestppd resolves *cupsFilter paths on the live filesystem, so a clean
# result here would be misleading until the package is actually installed.
# Check everything except that one expected failure.
if cupstestppd "${ROOT}${PPD_DIR}/canongm2080-native.ppd" 2>&1 |
       grep -E '\*\*FAIL\*\*' | grep -qv 'cupsFilter'; then
    echo "PPD has failures beyond the not-yet-installed filter:" >&2
    cupstestppd "${ROOT}${PPD_DIR}/canongm2080-native.ppd" >&2 || true
    exit 1
fi
echo "    PPD OK"

# Strip extended attributes: otherwise pkgbuild carries AppleDouble "._"
# companion files for every staged file into the payload.
xattr -cr "$ROOT"

say "Building package"
pkgbuild \
    --root "$ROOT" \
    --identifier "$IDENTIFIER" \
    --version "$VERSION" \
    --ownership recommended \
    --scripts "${HERE}/pkg-scripts" \
    --install-location / \
    "${DIST}/CanonGM2080Native-${VERSION}.pkg"

say "Done"
ls -lh "${DIST}/CanonGM2080Native-${VERSION}.pkg"
cat <<SUMMARY

    Install with:
        sudo installer -pkg "${DIST}/CanonGM2080Native-${VERSION}.pkg" -target /

    The package is unsigned, so double-clicking it will be blocked by
    Gatekeeper. The installer command above is the intended path.

    Then add the printer:
        ./add-printer.sh <printer-ip>
SUMMARY
