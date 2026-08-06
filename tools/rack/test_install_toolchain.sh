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
