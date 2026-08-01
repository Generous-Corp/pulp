#!/usr/bin/env bash
# Sign, notarize and staple the four Forge Modular bundles.
#
#   sign_bundles.sh              # sign, notarize, staple, verify
#   sign_bundles.sh --sign-only  # sign and verify; no submission (offline)
#   sign_bundles.sh --check      # report what each bundle carries, change nothing
#
# This exists because the step was done by hand and went wrong in the way
# hand-done steps do: a rebuild replaced four notarized bundles with ad-hoc
# ones, the copy to the other machine succeeded, and nothing said so until a
# `codesign --verify` was run against a record of what had been there before.
# An ad-hoc bundle is refused by Gatekeeper on any machine that did not build
# it, which reads as "the plugin is broken".
#
# Credentials come from ~/.config/pulp/secrets/, never from the repository, and
# never from the login keychain -- signing out of the login keychain is what
# pops the "allow access" dialog that wedges an unattended run. Environment
# variables of the same name win, so CI can supply them another way.

set -uo pipefail

BUILD="${FORGE_BUILD:-/tmp/forge-cur/build}"
SECRETS="${PULP_SECRETS_DIR:-$HOME/.config/pulp/secrets}"
MODE=sign
case "${1:-}" in
    --check)     MODE=check ;;
    --sign-only) MODE=signonly ;;
    "")          ;;
    *) echo "unknown argument: $1" >&2; exit 2 ;;
esac

say()  { printf '  %s\n' "$*"; }
step() { printf '\n%s\n' "$*"; }

BUNDLES=(
    "$BUILD/AU/Forge Modular.component"
    "$BUILD/VST3/Forge Modular.vst3"
    "$BUILD/CLAP/Forge Modular.clap"
    "$BUILD/modular/Forge Modular.app"
)

step "1. what each bundle carries now"
missing=0
for b in "${BUNDLES[@]}"; do
    if [ ! -e "$b" ]; then
        say "MISSING: $b"
        missing=1
        continue
    fi
    auth=$(codesign -dv --verbose=2 "$b" 2>&1 | grep -E '^Authority=' | head -1)
    auth="${auth#Authority=}"
    # An ad-hoc signature has no Authority line at all. Saying "ad-hoc" rather
    # than printing nothing is the difference between a report that is read and
    # one that is skimmed past, which is how this went wrong the first time.
    [ -n "$auth" ] || auth="AD-HOC — Gatekeeper will refuse this elsewhere"
    stapled=$(xcrun stapler validate "$b" 2>&1 | grep -qi 'validated' \
              && echo "stapled" || echo "not stapled")
    say "$(basename "$b"): $auth; $stapled"
done
[ "$missing" -eq 0 ] || { echo; echo "build them first"; exit 1; }
[ "$MODE" = check ] && { step "check only; nothing was changed"; exit 0; }

step "2. credentials"
for f in keychain.env notary.env; do
    if [ -f "$SECRETS/$f" ]; then
        set -a; . "$SECRETS/$f"; set +a
        say "read $f"
    else
        say "no $SECRETS/$f — relying on the environment"
    fi
done
: "${PULP_SIGN_IDENTITY_HASH:?no signing identity: set PULP_SIGN_IDENTITY_HASH or provide keychain.env}"
say "identity ${PULP_SIGN_IDENTITY_HASH:0:8}…"

# Unlock the dedicated signing keychain and authorize codesign against it, so
# nothing prompts. Never the login keychain.
if [ -n "${PULP_SIGN_KEYCHAIN:-}" ] && [ -f "${PULP_SIGN_KEYCHAIN}" ]; then
    security unlock-keychain -p "${PULP_SIGN_KEYCHAIN_PW:-}" "$PULP_SIGN_KEYCHAIN" \
        && say "unlocked $(basename "$PULP_SIGN_KEYCHAIN")"
    KEYCHAIN_ARG=(--keychain "$PULP_SIGN_KEYCHAIN")
else
    KEYCHAIN_ARG=()
fi

step "3. signing"
# Inner Mach-O first, container last. A bundle signed before the dylib inside it
# is invalid the moment the dylib is signed, and `codesign --verify` on the
# container is what notices -- after the copy, if nobody checks.
sign_one() {
    codesign --force --sign "$PULP_SIGN_IDENTITY_HASH" \
             "${KEYCHAIN_ARG[@]}" \
             --options runtime --timestamp "$1" 2>&1 | sed 's/^/      /'
}
for b in "${BUNDLES[@]}"; do
    while IFS= read -r inner; do
        [ -n "$inner" ] || continue
        sign_one "$inner"
    done < <(find "$b" \( -name '*.dylib' -o -name '*.so' \) -type f)
    sign_one "$b"
    if codesign --verify --deep --strict "$b" 2>/dev/null; then
        say "signed  $(basename "$b")"
    else
        say "SIGNING FAILED  $(basename "$b")"
        exit 1
    fi
done

if [ "$MODE" = signonly ]; then
    step "signed, not notarized (--sign-only). Gatekeeper on another machine"
    step "will still refuse these until they are notarized and stapled."
    exit 0
fi

step "4. notarizing"
: "${PULP_NOTARY_KEY_PATH:?no notary key: provide notary.env}"
: "${PULP_NOTARY_KEY_ID:?no notary key id}"
: "${PULP_NOTARY_ISSUER_ID:?no notary issuer}"
# notarytool takes an upload container, not a bundle directory. One zip of all
# four is one submission and one wait, rather than four of each.
ZIP="${TMPDIR:-/tmp}/forge-modular-notarize.zip"
rm -f "$ZIP"
( cd "$BUILD" && ditto -c -k --keepParent --sequesterRsrc \
    "AU/Forge Modular.component" "${TMPDIR:-/tmp}/fm-au.zip" ) || exit 1
# ditto takes one source, so each bundle is zipped and the zips are submitted
# together in a parent zip -- notarytool notarizes what it finds inside.
for pair in "AU/Forge Modular.component:fm-au" \
            "VST3/Forge Modular.vst3:fm-vst3" \
            "CLAP/Forge Modular.clap:fm-clap" \
            "modular/Forge Modular.app:fm-app"; do
    src="${pair%%:*}"; name="${pair##*:}"
    ( cd "$BUILD" && ditto -c -k --keepParent --sequesterRsrc \
        "$src" "${TMPDIR:-/tmp}/$name.zip" ) || exit 1
done
( cd "${TMPDIR:-/tmp}" && ditto -c -k --sequesterRsrc \
    fm-au.zip "$ZIP" ) >/dev/null 2>&1 || true

submit() { # <zip>
    xcrun notarytool submit "$1" \
        --key "$PULP_NOTARY_KEY_PATH" \
        --key-id "$PULP_NOTARY_KEY_ID" \
        --issuer "$PULP_NOTARY_ISSUER_ID" --wait 2>&1
}
ok=1
for pair in "AU/Forge Modular.component:fm-au" \
            "VST3/Forge Modular.vst3:fm-vst3" \
            "CLAP/Forge Modular.clap:fm-clap" \
            "modular/Forge Modular.app:fm-app"; do
    src="${pair%%:*}"; name="${pair##*:}"
    out=$(submit "${TMPDIR:-/tmp}/$name.zip")
    status=$(printf '%s\n' "$out" | grep -E '^ +status:' | tail -1 | awk '{print $2}')
    if [ "$status" = "Accepted" ]; then
        say "notarized $(basename "$src")"
        # Staple the BUNDLE, not the zip: the ticket has to travel with what is
        # copied, and a stapled zip is thrown away.
        if xcrun stapler staple "$BUILD/$src" >/dev/null 2>&1; then
            say "  stapled"
        else
            say "  STAPLE FAILED — the bundle needs the network to validate"
            ok=0
        fi
    else
        say "NOTARIZATION $status for $(basename "$src")"
        printf '%s\n' "$out" | tail -5 | sed 's/^/      /'
        ok=0
    fi
done

step "5. what each bundle carries now"
for b in "${BUNDLES[@]}"; do
    auth=$(codesign -dv --verbose=2 "$b" 2>&1 | grep -E '^Authority=' | head -1)
    auth="${auth#Authority=}"
    [ -n "$auth" ] || auth="AD-HOC"
    stapled=$(xcrun stapler validate "$b" 2>&1 | grep -qi 'validated' \
              && echo "stapled" || echo "NOT stapled")
    say "$(basename "$b"): $auth; $stapled"
done

[ "$ok" -eq 1 ] || exit 1
step "done. These can be copied to another Mac; setup_m5.sh step 7 re-checks."
