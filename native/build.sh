#!/usr/bin/env bash
#
# Builds the native driver and wraps it in a macOS installer package.
#
# Unsigned by default:
#     ./build.sh
#
# Signed and notarized (needs a paid Apple Developer account):
#     SIGN_APP="Developer ID Application: Your Name (TEAMID)" \
#     SIGN_PKG="Developer ID Installer: Your Name (TEAMID)" \
#     NOTARY_PROFILE=canon-gm2080 \
#     ./build.sh
#
# NOTARY_PROFILE names a keychain item you create once, interactively:
#
#     xcrun notarytool store-credentials canon-gm2080 \
#         --apple-id you@example.com --team-id TEAMID
#
# That command prompts for an app-specific password (appleid.apple.com ->
# Sign-In and Security -> App-Specific Passwords) and stores it in your
# keychain.  This script never sees it, and no credential should ever be
# written into this file or passed on its command line, where it would land
# in your shell history and in the process list.
#
# Output: dist/CanonGM2080Native-<version>.pkg
set -euo pipefail

VERSION="${VERSION:-1.0.1}"
IDENTIFIER="com.local.canon-gm2080-native"

SIGN_APP="${SIGN_APP:-}"
SIGN_PKG="${SIGN_PKG:-}"
NOTARY_PROFILE="${NOTARY_PROFILE:-}"

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD="${HERE}/build"
ROOT="${BUILD}/pkgroot"
DIST="${HERE}/dist"
PKG="${DIST}/CanonGM2080Native-${VERSION}.pkg"

FILTER_DIR="/Library/Printers/canon-gm2080"
PPD_DIR="/Library/Printers/PPDs/Contents/Resources"

say()  { printf '\n\033[1m==> %s\033[0m\n' "$*"; }
info() { printf '    %s\n' "$*"; }
die()  { printf '\n\033[31mERROR: %s\033[0m\n' "$*" >&2; exit 1; }

# --------------------------------------------------------------- preflight

# Fail before a long build rather than after it: a missing certificate or a
# typo'd identity name is much cheaper to report now.
have_identity() {
    security find-identity -v 2>/dev/null | grep -qF "$1"
}

if [[ -n "$SIGN_APP" ]] && ! have_identity "$SIGN_APP"; then
    say "Available signing identities"
    security find-identity -v 2>/dev/null | sed 's/^/    /' || true
    die "SIGN_APP identity not found in the keychain: ${SIGN_APP}"
fi

if [[ -n "$SIGN_PKG" ]] && ! have_identity "$SIGN_PKG"; then
    say "Available signing identities"
    security find-identity -v 2>/dev/null | sed 's/^/    /' || true
    die "SIGN_PKG identity not found in the keychain: ${SIGN_PKG}"
fi

# Notarization is only meaningful for a signed package, and Apple rejects a
# package whose payload executables are not themselves Developer ID signed.
if [[ -n "$NOTARY_PROFILE" ]]; then
    [[ -n "$SIGN_PKG" ]] || die "NOTARY_PROFILE set but SIGN_PKG is not: Apple only notarizes signed packages"
    [[ -n "$SIGN_APP" ]] || die "NOTARY_PROFILE set but SIGN_APP is not: the filter binary inside the package must be signed too"

    if ! xcrun notarytool history --keychain-profile "$NOTARY_PROFILE" \
             --limit 1 >/dev/null 2>&1; then
        die "no usable notarytool credentials for profile '${NOTARY_PROFILE}'. Create it with:
    xcrun notarytool store-credentials ${NOTARY_PROFILE} --apple-id <you> --team-id <TEAMID>"
    fi
    info "notarytool profile '${NOTARY_PROFILE}' OK"
fi

rm -rf "$BUILD"
mkdir -p "${ROOT}${FILTER_DIR}" "${ROOT}${PPD_DIR}" "$DIST"

# ----------------------------------------------------------------- compile

# rastertocanonijgm renders pages; cmdtocanonijgm handles maintenance
# (head cleaning, nozzle check, alignment).  Both share the IVEC envelope.
FILTERS=(rastertocanonijgm cmdtocanonijgm)

say "Compiling filters (universal)"
for f in "${FILTERS[@]}"; do
    clang -O2 -Wall -Wextra -Wno-deprecated-declarations \
          -arch arm64 -arch x86_64 \
          -o "${ROOT}${FILTER_DIR}/${f}" \
          "${HERE}/src/${f}.c" "${HERE}/src/ivec.c" -lcups
    # CUPS refuses to run a filter that is group- or world-writable.
    chmod 755 "${ROOT}${FILTER_DIR}/${f}"
    info "$(lipo -info "${ROOT}${FILTER_DIR}/${f}")"
done

if [[ -n "$SIGN_APP" ]]; then
    say "Signing the filter binaries"
    for f in "${FILTERS[@]}"; do
        # --timestamp and --options runtime are both notarization
        # requirements, not optional hardening.
        codesign --force \
                 --sign "$SIGN_APP" \
                 --identifier "${IDENTIFIER}.${f}" \
                 --options runtime \
                 --timestamp \
                 "${ROOT}${FILTER_DIR}/${f}"
        codesign --verify --strict "${ROOT}${FILTER_DIR}/${f}"
        info "signed ${f}"
    done
fi

# --------------------------------------------------------------------- ppd

say "Generating PPDs"
# One template per GM series; capabilities are identical, only identity
# strings differ.  See ppd/generate.sh and docs/models.md.
GENERATED_SERIES=$("${HERE}/ppd/generate.sh" "${ROOT}${PPD_DIR}")
for s in $GENERATED_SERIES; do
    lower="$(echo "$s" | tr '[:upper:]' '[:lower:]')"
    chmod 644 "${ROOT}${PPD_DIR}/canon${lower}-native.ppd"
    info "canon${lower}-native.ppd"
done

say "Validating PPDs"
# cupstestppd resolves *cupsFilter paths against the live filesystem, so it
# always reports the not-yet-installed filters as missing.  Ignore only those
# failures - ignoring all of them would make this check meaningless.
for ppd in "${ROOT}${PPD_DIR}"/canon*-native.ppd; do
    if LC_ALL=C cupstestppd "$ppd" 2>&1 |
           grep -E '\*\*FAIL\*\*' | grep -qv 'cupsFilter'; then
        LC_ALL=C cupstestppd "$ppd" >&2 || true
        die "$(basename "$ppd") has failures beyond the not-yet-installed filters"
    fi
done
info "all PPDs OK"

# ----------------------------------------------------------------- package

# Clears quarantine and similar attributes.  It does NOT remove
# com.apple.provenance, which macOS re-applies and pkgbuild always archives -
# which is why `pkgutil --payload-files` shows a "._" entry beside every file.
# Those are AppleDouble-encoded attributes, not real files: the installer
# decodes them, and `pkgutil --expand-full` confirms the installed tree
# contains only the three intended files.
xattr -cr "$ROOT" 2>/dev/null || true

say "Building package"
pkgbuild_args=(
    --root "$ROOT"
    --identifier "$IDENTIFIER"
    --version "$VERSION"
    --ownership recommended
    --scripts "${HERE}/pkg-scripts"
    --install-location /
)
[[ -n "$SIGN_PKG" ]] && pkgbuild_args+=(--sign "$SIGN_PKG")

pkgbuild "${pkgbuild_args[@]}" "$PKG"

if [[ -n "$SIGN_PKG" ]]; then
    pkgutil --check-signature "$PKG" | sed 's/^/    /'
fi

# --------------------------------------------------------------- notarize

if [[ -n "$NOTARY_PROFILE" ]]; then
    say "Submitting for notarization (this waits on Apple; usually 1-5 min)"

    # Capture the id so the log can be fetched whether or not this succeeds -
    # "Invalid" without the log tells you nothing about what Apple objected to.
    submit_out="$(mktemp -t gmnotary)"
    if xcrun notarytool submit "$PKG" \
           --keychain-profile "$NOTARY_PROFILE" \
           --wait 2>&1 | tee "$submit_out"; then
        status="$(grep -oE '  status: [A-Za-z]+' "$submit_out" | tail -1 | awk '{print $2}')"
    else
        status="submit-failed"
    fi

    submission_id="$(grep -oE '  id: [0-9a-f-]{36}' "$submit_out" | head -1 | awk '{print $2}')"
    rm -f "$submit_out"

    if [[ "$status" != "Accepted" ]]; then
        if [[ -n "$submission_id" ]]; then
            say "Notarization log"
            xcrun notarytool log "$submission_id" \
                  --keychain-profile "$NOTARY_PROFILE" 2>&1 | sed 's/^/    /' || true
        fi
        die "notarization did not succeed (status: ${status:-unknown})"
    fi

    say "Stapling the ticket"
    # Stapling embeds the ticket so the package validates on a machine with no
    # network access; without it Gatekeeper has to reach Apple at install time.
    xcrun stapler staple "$PKG"
    xcrun stapler validate "$PKG"

    say "Verifying as Gatekeeper will see it"
    spctl --assess --type install --verbose=4 "$PKG"
fi

# -------------------------------------------------------------------- done

say "Done"
ls -lh "$PKG"

if [[ -n "$NOTARY_PROFILE" ]]; then
    cat <<SUMMARY

    Signed, notarized and stapled. It will install by double-click on any Mac.

        sudo installer -pkg "${PKG}" -target /
        ./add-printer.sh <printer-ip>
SUMMARY
elif [[ -n "$SIGN_PKG" ]]; then
    cat <<SUMMARY

    Signed but NOT notarized. Gatekeeper still blocks double-clicking on
    machines other than this one. Set NOTARY_PROFILE to complete the chain.

        sudo installer -pkg "${PKG}" -target /
        ./add-printer.sh <printer-ip>
SUMMARY
else
    cat <<SUMMARY

    Unsigned. Double-clicking is blocked by Gatekeeper; installing from the
    command line is the intended path and works fine:

        sudo installer -pkg "${PKG}" -target /
        ./add-printer.sh <printer-ip>

    To sign and notarize instead, see the header of this script.
SUMMARY
fi
