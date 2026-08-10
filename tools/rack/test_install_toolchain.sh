#!/usr/bin/env bash
# The installer, run against homes it has never seen.
#
# This is the rehearsal for setting up a new machine. The installer had never
# actually been run against a fresh home: every run on this machine landed on a
# directory that already held generated state, so the case that matters for a
# new machine -- the ONLY case that matters for a new machine -- was the one
# never exercised. It failed, and would have failed on the M5.
#
# Both directions are asserted: committed built-ins refresh as one release,
# while explicitly generated modules and patches survive that refresh.
#
#   tools/rack/test_install_toolchain.sh

set -uo pipefail

HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
INSTALL="$HERE/install_toolchain.sh"
pass=0
fail=0

ok()   { printf '  ok     %s\n' "$1"; pass=$((pass + 1)); }
bad()  { printf '  WRONG  %s\n' "$1"; fail=$((fail + 1)); }

# --- broad and redirected destinations fail before recursive mutation --------
UNSAFE="$(mktemp -d)"
mkdir -p "$UNSAFE/home" "$UNSAFE/target"
if HOME="$UNSAFE/home" FORGE_MODULAR_HOME="$UNSAFE/home" \
       "$INSTALL" >/dev/null 2>&1; then
    bad "the user's home cannot become the recursive toolchain destination"
else
    ok "the user's home cannot become the recursive toolchain destination"
fi
ln -s "$UNSAFE/target" "$UNSAFE/link"
if FORGE_MODULAR_HOME="$UNSAFE/link" "$INSTALL" >/dev/null 2>&1; then
    bad "a symlink cannot become the recursive toolchain destination"
else
    ok "a symlink cannot become the recursive toolchain destination"
fi
if [ -z "$(find "$UNSAFE/target" -mindepth 1 -print -quit)" ]; then
    ok "rejected destinations are untouched"
else
    bad "a rejected destination was mutated"
fi
rm -rf "$UNSAFE"

# --- a fresh home installs and can emit a panel -------------------------------
FRESH="$(mktemp -d)"
out="$(FORGE_MODULAR_HOME="$FRESH" "$INSTALL" 2>&1)"
code=$?
if [ "$code" -eq 0 ] && printf '%s' "$out" | grep -q "toolchain is complete"; then
    ok "a fresh home installs and emits a panel"
else
    bad "a fresh home installs and emits a panel (exit $code)"
    printf '%s\n' "$out" | tail -6 | sed 's/^/         /'
fi

# The seed has to be the thing the verification needs, not merely present.
if [ -f "$FRESH/examples/forge-modular/modules/_plugin.json" ]; then
    ok "a fresh home is seeded with the module manifests"
else
    bad "a fresh home is seeded with the module manifests"
fi
if [ -f "$FRESH/core/timebase/include/pulp/timebase/beat_division.hpp" ]; then
    ok "a fresh home includes signal's public timebase dependency"
else
    bad "a fresh home omits signal's public timebase dependency"
fi

# Replay a retained, deterministic response through the installed generator.
# This compiles every shipped source plus a newly emitted module, drives its
# real process() gate, and packages it without resolving or invoking a model.
REPLAY="$FRESH/tools/rack/fixtures/toolchain_compile_response.txt"
PROMPT="Create a new 6HP clock module named TOOLCHAINPROBE. RATE -3..3 default 0; WIDTH 0.05..0.95 default 0.5; normalled RATE CV input; RESET input; CLOCK 10V output; PHASE 0..10V output."
replay_out="$(FORGE_MODEL_PROVIDER=not-a-provider \
    FORGE_ATTEMPT_DIR="$FRESH/attempts" \
    python3 "$FRESH/tools/rack/generate.py" "$PROMPT" \
    --retries 9 --response-file "$REPLAY" \
    --install-dir "$FRESH/replay-install" 2>&1)"
replay_code=$?
if [ "$replay_code" -eq 0 ] \
        && printf '%s\n' "$replay_out" | grep -q '^[[:space:]]*compiled$' \
        && printf '%s\n' "$replay_out" | grep -q '^[[:space:]]*behaviour verified$' \
        && [ -f "$FRESH/examples/forge-modular/src/TOOLCHAINPROBE.cpp" ] \
        && find "$FRESH/replay-install" -maxdepth 1 -name '*.vcvplugin' \
             -type f -print -quit | grep -q .; then
    ok "the clean installed pack and a retained-response module compile without a provider"
else
    bad "the clean installed pack and retained-response module compile (exit $replay_code)"
    printf '%s\n' "$replay_out" | tail -12 | sed 's/^/         /'
fi

# A checkout has no release stamp. Refreshing from one must leave whichever
# installed stamp it found alone; release-vs-working-copy selection must not be
# changed by a developer refresh.
printf '0.12.8\ncheckout-preserve\n' \
    > "$FRESH/tools/rack/FORGE_TOOLCHAIN_STAMP"
printf '0.12.7\nlegacy-preserve\n' > "$FRESH/tools/rack/VERSION"

# --- an existing home keeps its generated work --------------------------------
# The regression behind the exclusions: a module built from the app opened in
# Rack once, and the next reinstall removed its .vcv.
echo '{"generated":true}' > "$FRESH/examples/forge-modular/patches/probe.vcv"
sed '1a\
  "forge_generated": true,' \
    "$HERE/../../examples/forge-modular/modules/atten.json" \
    | sed 's/"slug": "ATTEN"/"slug": "PROBEMOD"/' \
    > "$FRESH/examples/forge-modular/modules/probemod.json"
cp "$HERE/../../examples/forge-modular/src/ATTEN.cpp" \
   "$FRESH/examples/forge-modular/src/PROBEMOD.cpp"
printf '{"stale":true}\n' > "$FRESH/examples/forge-modular/modules/atten.json"
FORGE_MODULAR_HOME="$FRESH" "$INSTALL" >/dev/null 2>&1
code=$?
if [ "$code" -ne 0 ]; then
    bad "a second install succeeds (exit $code)"
else
    ok "a second install succeeds"
fi
if grep -q '^0.12.8$' "$FRESH/tools/rack/FORGE_TOOLCHAIN_STAMP" \
        && grep -q '^0.12.7$' "$FRESH/tools/rack/VERSION"; then
    ok "a checkout refresh preserves installed current and legacy stamps"
else
    bad "a checkout refresh changed installed release-selection stamps"
fi
if [ -f "$FRESH/examples/forge-modular/patches/probe.vcv" ]; then
    ok "a reinstall keeps a generated patch"
else
    bad "a reinstall keeps a generated patch — it was deleted"
fi
if [ -f "$FRESH/examples/forge-modular/modules/probemod.json" ]; then
    ok "a reinstall keeps a generated module manifest"
else
    bad "a reinstall keeps a generated module manifest — it was deleted"
fi
if cmp -s "$FRESH/examples/forge-modular/modules/atten.json" \
          "$HERE/../../examples/forge-modular/modules/atten.json"; then
    ok "a reinstall refreshes committed built-in manifests"
else
    bad "a stale built-in survived under the new toolchain version"
fi

# --- a newly stamped toolchain migrates only the collision-prone old stamp --
# Fabricate the bundle-shaped source tree around the real inputs. This drives
# the installer rather than grepping its text: the source has the new stamp,
# the destination has the old one, and generated/private state already exists.
STAGED="$(mktemp -d)"
mkdir -p "$STAGED/tools/rack" "$STAGED/tools" "$STAGED/docs/status" \
         "$STAGED/external" "$STAGED/examples" "$STAGED/build"
rsync -a --exclude __pycache__ --exclude '*.pyc' \
    "$HERE/" "$STAGED/tools/rack/"
ln -s "$HERE/../dsp_vocabulary.py" "$STAGED/tools/dsp_vocabulary.py"
ln -s "$HERE/../../docs/status/agent-capabilities.json" \
    "$STAGED/docs/status/agent-capabilities.json"
ln -s "$HERE/../../external/fonts" "$STAGED/external/fonts"
ln -s "$HERE/../../examples/forge-modular" \
    "$STAGED/examples/forge-modular"
for component in signal format audio state platform runtime timebase; do
    mkdir -p "$STAGED/core/$component"
    ln -s "$HERE/../../core/$component/include" \
        "$STAGED/core/$component/include"
done
cp "$HERE/../../build/shape_text" "$STAGED/build/shape_text"
printf '0.13.0\n2026-08-06T00:00:00Z\n' \
    > "$STAGED/tools/rack/FORGE_TOOLCHAIN_STAMP"
mkdir -p "$FRESH/private"
printf 'private-user-state\n' > "$FRESH/private/catalog.sqlite3"

FORGE_MODULAR_HOME="$FRESH" "$STAGED/tools/rack/install_toolchain.sh" \
    >/dev/null 2>&1
code=$?
if [ "$code" -eq 0 ]; then
    ok "a newly stamped toolchain installs over a legacy-stamped home"
else
    bad "a newly stamped toolchain installs over a legacy-stamped home (exit $code)"
fi
if [ -f "$FRESH/tools/rack/FORGE_TOOLCHAIN_STAMP" ] \
        && [ ! -e "$FRESH/tools/rack/VERSION" ]; then
    ok "the new stamp replaces only the known legacy VERSION filename"
else
    bad "the new stamp did not remove the known legacy VERSION filename"
fi
if [ -f "$FRESH/private/catalog.sqlite3" ] \
        && [ -f "$FRESH/examples/forge-modular/patches/probe.vcv" ] \
        && [ -f "$FRESH/examples/forge-modular/modules/probemod.json" ]; then
    ok "stamp migration preserves private state, patches, and generated modules"
else
    bad "stamp migration removed private or generated user state"
fi

# This is the packaging regression itself. On case-insensitive macOS, adding
# tools/rack to the include path made libc++ <version> resolve to VERSION and
# fed the release number to the C++ parser. The non-colliding stamp must coexist
# with that include path while the standard header still compiles.
if printf '%s\n' '#include <version>' \
        'static_assert(__cplusplus >= 202002L);' \
        | clang++ -std=c++20 -I"$FRESH/tools/rack" -x c++ -fsyntax-only -; then
    ok "a stamped toolchain root still compiles libc++ <version>"
else
    bad "the toolchain stamp still collides with libc++ <version>"
fi
rm -rf "$STAGED"

# --- an interrupted first install still gets seeded ---------------------------
# modules/ existing but empty is what a cancelled install leaves behind, and it
# is exactly the machine that still needs seeding. Keying the seed on the
# directory rather than on the manifest inside it would strand this one.
PARTIAL="$(mktemp -d)"
mkdir -p "$PARTIAL/examples/forge-modular/modules"
FORGE_MODULAR_HOME="$PARTIAL" "$INSTALL" >/dev/null 2>&1
if [ -f "$PARTIAL/examples/forge-modular/modules/_plugin.json" ]; then
    ok "an interrupted first install is seeded on the next run"
else
    bad "an interrupted first install is seeded on the next run"
fi

rm -rf "$FRESH" "$PARTIAL"
printf '\n%d/%d checks passed\n' "$pass" "$((pass + fail))"
[ "$fail" -eq 0 ]
