#!/bin/sh
# Shell out to the built pulp-cpp with a stub `vellum` and check that `pulp ui`
# forwards its arguments and exit status, and that `pulp import-design`
# without the importer names the `vellum import` spelling instead.
set -eu
pulp_cpp=$1
work=$(mktemp -d "${TMPDIR:-/tmp}/pulp-vellum-forward.XXXXXX")
trap 'rm -rf "$work"' EXIT
fail() { echo "FAIL: $*" >&2; exit 1; }

# A copy outside any build tree has no sibling importer to find.
mkdir -p "$work/bin" "$work/cwd"
cp "$pulp_cpp" "$work/bin/pulp-cpp"
cat > "$work/vellum" <<'STUB'
#!/bin/sh
printf '%s\n' "$@" > "$VELLUM_STUB_ARGV"
exit 7
STUB
chmod +x "$work/vellum"

cd "$work/cwd"
status=0
VELLUM_STUB_ARGV="$work/argv" PULP_VELLUM_CLI="$work/vellum" \
  "$work/bin/pulp-cpp" ui build --source "native ui/src" --out out > "$work/out" 2>&1 || status=$?
[ "$status" = 7 ] || fail "pulp ui returned $status, expected the stub's 7: $(cat "$work/out")"
printf '%s\n' ui build --source "native ui/src" --out out > "$work/expected"
cmp -s "$work/argv" "$work/expected" || fail "forwarded argv differs: $(cat "$work/argv")"

status=0
PULP_VELLUM_CLI="$work/missing-vellum" PATH=/usr/bin:/bin \
  "$work/bin/pulp-cpp" ui build > "$work/out" 2>&1 || status=$?
[ "$status" = 2 ] || fail "pulp ui without vellum returned $status, expected 2"
grep -q "provided by the Vellum CLI, which was not found" "$work/out" ||
  fail "missing-vellum message absent: $(cat "$work/out")"

rm -f "$work/argv"
status=0
VELLUM_STUB_ARGV="$work/argv" PULP_VELLUM_CLI="$work/vellum" \
  "$work/bin/pulp-cpp" import-design --from pencil --file panel.pen > "$work/out" 2>&1 || status=$?
[ "$status" = 2 ] || fail "import-design returned $status, expected 2: $(cat "$work/out")"
grep -q "Equivalent: vellum import pencil panel.pen" "$work/out" ||
  fail "import-design spelling absent: $(cat "$work/out")"
[ ! -e "$work/argv" ] || fail "import-design must not forward its arguments"
echo "PASS: pulp ui forwards to vellum; import-design names vellum import"
