#!/usr/bin/env bash
# Hermetic contract coverage for staged control-broker activation.

set -euo pipefail

PULP_SRC="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL_SH="${PULP_INSTALL_SH_UNDER_TEST:-$PULP_SRC/tools/install/install.sh}"
TEST_ROOT="$(mktemp -d)"
trap 'rm -rf "$TEST_ROOT"' EXIT

PASS=0
FAIL=0

pass() {
    echo "  PASS — $1"
    PASS=$((PASS + 1))
}

fail() {
    echo "  FAIL — $1"
    FAIL=$((FAIL + 1))
}

assert_file_contains() {
    local label="$1" file="$2" expected="$3"
    if [[ -f "$file" ]] && grep -qF -- "$expected" "$file"; then
        pass "$label"
    else
        fail "$label (missing '$expected' in $file)"
    fi
}

assert_file_excludes() {
    local label="$1" file="$2" unexpected="$3"
    if [[ ! -f "$file" ]] || ! grep -qF -- "$unexpected" "$file"; then
        pass "$label"
    else
        fail "$label (unexpected '$unexpected' in $file)"
    fi
}

assert_absent() {
    local label="$1" path="$2"
    if [[ ! -e "$path" ]]; then
        pass "$label"
    else
        fail "$label (unexpected path $path)"
    fi
}

make_fixture_archive() {
    local payload="$TEST_ROOT/payload"
    mkdir -p "$payload"
    cat > "$payload/pulp" <<'EOF'
#!/bin/sh
case "${1:-}" in
    __control-broker-reconcile)
        broker_contents=missing
        host_contents=missing
        manifest_contents=missing
        runtime_contents=missing
        if [ "${2:-}" = "--broker" ] && [ -f "${3:-}" ]; then
            broker_contents=$(cat "$3")
        fi
        if [ "${4:-}" = "--standalone-host" ] && [ -f "${5:-}" ]; then
            host_contents=$(cat "$5")
        fi
        if [ "${6:-}" = "--standalone-manifest" ] && [ -f "${7:-}" ]; then
            manifest_contents=$(cat "$7")
        fi
        if [ "${8:-}" = "--standalone-runtime" ] && [ -f "${9:-}" ]; then
            runtime_contents=$(cat "$9")
        fi
        printf 'reconcile|argc=%s|arg2=%s|broker=%s|arg4=%s|arg5=%s|arg6=%s|arg7=%s|arg8=%s|arg9=%s|arg10=%s|contents=%s|host=%s|manifest=%s|runtime=%s\n' \
            "$#" "${2:-}" "${3:-}" "${4:-}" "${5:-}" "${6:-}" "${7:-}" \
            "${8:-}" "${9:-}" "${10:-}" "$broker_contents" "$host_contents" "$manifest_contents" "$runtime_contents" \
            >> "$MOCK_PULP_LOG"
        if [ "${MOCK_REJECT_UNACCEPTED_CUSTOM:-0}" = "1" ] && \
           [ "${10:-}" != "--accept-custom-root" ]; then
            exit 66
        fi
        ;;
    --version)
        printf 'version|argc=%s\n' "$#" >> "$MOCK_PULP_LOG"
        echo "0.0.0-test"
        ;;
esac
EOF
    chmod +x "$payload/pulp"
    printf 'broker-fixture\n' > "$payload/pulp-control-broker"
    printf 'host-fixture\n' > "$payload/pulp-control-standalone-host"
    printf '{"schema_version":1}\n' > "$payload/pulp-control-standalone-host.inspector-capabilities.json"
    printf 'runtime-fixture\n' > "$payload/libwgpu_native.dylib"
    tar -czf "$TEST_ROOT/pulp.tar.gz" -C "$payload" \
        pulp pulp-control-broker pulp-control-standalone-host \
        pulp-control-standalone-host.inspector-capabilities.json libwgpu_native.dylib
}

make_command_shims() {
    local mock_bin="$TEST_ROOT/mock-bin"
    mkdir -p "$mock_bin"

    cat > "$mock_bin/uname" <<'EOF'
#!/bin/sh
case "${1:-}" in
    -s) echo Darwin ;;
    -m) echo arm64 ;;
    *) echo Darwin ;;
esac
EOF

    cat > "$mock_bin/curl" <<'EOF'
#!/bin/sh
output=
while [ "$#" -gt 0 ]; do
    if [ "$1" = "-o" ]; then
        shift
        output="$1"
    fi
    shift
done
[ -n "$output" ] || exit 2
cp "$MOCK_ARCHIVE" "$output"
EOF

    for command in codesign plutil launchctl; do
        cat > "$mock_bin/$command" <<'EOF'
#!/bin/sh
printf '%s\n' "$(basename "$0") $*" >> "$MOCK_SENSITIVE_LOG"
exit 97
EOF
    done

    chmod +x "$mock_bin"/*
}

run_installer() {
    local case_root="$1" install_dir="$2"
    shift 2
    mkdir -p "$case_root/home"
    env \
        HOME="$case_root/home" \
        PATH="$TEST_ROOT/mock-bin:$PATH" \
        MOCK_ARCHIVE="$TEST_ROOT/pulp.tar.gz" \
        MOCK_PULP_LOG="$case_root/pulp.log" \
        MOCK_SENSITIVE_LOG="$case_root/sensitive.log" \
        PULP_VERSION=0.0.0-test \
        PULP_INSTALL_DIR="$install_dir" \
        PULP_NO_MODIFY_PATH=1 \
        PULP_SKIP_SDK_INSTALL=1 \
        "$@" \
        bash "$INSTALL_SH" >"$case_root/output.log" 2>&1
}

assert_common_contract() {
    local label="$1" case_root="$2" install_dir="$3"
    local log="$case_root/pulp.log"
    local reconcile_line version_line broker_path

    assert_file_contains "$label installs the CLI payload" \
        "$install_dir/pulp" '__control-broker-reconcile'
    assert_absent "$label keeps the broker out of the install root" \
        "$install_dir/pulp-control-broker"
    assert_absent "$label keeps the host out of the install root before reconciliation" \
        "$install_dir/pulp-control-standalone-host"
    assert_absent "$label keeps the manifest out of the install root before reconciliation" \
        "$install_dir/pulp-control-standalone-host.inspector-capabilities.json"
    assert_absent "$label keeps the runtime out of the install root before reconciliation" \
        "$install_dir/libwgpu_native.dylib"
    assert_file_contains "$label reconciles the extracted broker bytes" \
        "$log" 'contents=broker-fixture'
    assert_file_contains "$label passes the hidden broker option" \
        "$log" 'arg2=--broker'
    assert_file_contains "$label passes the hidden host option" \
        "$log" 'arg4=--standalone-host'
    assert_file_contains "$label passes the hidden manifest option" \
        "$log" 'arg6=--standalone-manifest'
    assert_file_contains "$label reconciles the extracted host bytes" \
        "$log" 'host=host-fixture'
    assert_file_contains "$label reconciles the extracted manifest bytes" \
        "$log" 'manifest={"schema_version":1}'
    assert_file_contains "$label passes the hidden runtime option" \
        "$log" 'arg8=--standalone-runtime'
    assert_file_contains "$label reconciles the extracted runtime bytes" \
        "$log" 'runtime=runtime-fixture'
    assert_file_excludes "$label never passes the install-root broker path" \
        "$log" "broker=$install_dir/pulp-control-broker"

    broker_path=$(sed -n 's/^reconcile|.*|broker=\([^|]*\)|.*/\1/p' "$log")
    case "$broker_path" in
        */control-broker-stage/pulp-control-broker)
            pass "$label passes the exact staged extraction path"
            ;;
        *)
            fail "$label broker path was not staged (got '$broker_path')"
            ;;
    esac

    reconcile_line=$(grep -n '^reconcile|' "$log" | cut -d: -f1)
    version_line=$(grep -n '^version|' "$log" | cut -d: -f1)
    if [[ -n "$reconcile_line" && -n "$version_line" && \
          "$reconcile_line" -lt "$version_line" ]]; then
        pass "$label reconciles before running the installed CLI version check"
    else
        fail "$label did not reconcile before the version check"
    fi

    if [[ ! -s "$case_root/sensitive.log" ]]; then
        pass "$label shell path does not invoke codesign, plutil, or launchctl"
    else
        fail "$label invoked a security-sensitive command: $(tr '\n' ' ' < "$case_root/sensitive.log")"
    fi
}

[[ -f "$INSTALL_SH" ]] || { echo "FAIL: missing $INSTALL_SH"; exit 2; }
make_fixture_archive
make_command_shims

installer_commands=$(grep -vE '^[[:space:]]*#' "$INSTALL_SH" || true)
if grep -qE '(^|[[:space:];|&])(/usr/bin/)?(codesign|plutil|launchctl)([[:space:];|&]|$)' \
        <<< "$installer_commands"; then
    fail "installer source directly invokes codesign, plutil, or launchctl"
else
    pass "installer source delegates codesign, plist, and launchd work"
fi

echo "Scenario: canonical install root"
canonical_root="$TEST_ROOT/canonical"
canonical_install="$canonical_root/home/.pulp/bin"
run_installer "$canonical_root" ""
assert_common_contract "canonical install" "$canonical_root" "$canonical_install"
assert_file_contains "canonical install passes only --broker and its value" \
    "$canonical_root/pulp.log" 'reconcile|argc=9|'
assert_file_contains "canonical install does not opt into a custom root" \
    "$canonical_root/pulp.log" '|arg10=|'

echo "Scenario: custom install root without opt-in"
unaccepted_root="$TEST_ROOT/unaccepted-custom"
unaccepted_install="$unaccepted_root/opt/pulp/bin"
if run_installer "$unaccepted_root" "$unaccepted_install" \
        MOCK_REJECT_UNACCEPTED_CUSTOM=1; then
    fail "custom install without opt-in must surface reconciliation refusal"
else
    pass "custom install without opt-in surfaces reconciliation refusal"
fi
assert_file_contains "unaccepted custom install does not invent the opt-in" \
    "$unaccepted_root/pulp.log" '|arg10=|'
assert_file_contains "unaccepted custom install reports activation failure" \
    "$unaccepted_root/output.log" 'control broker activation failed'

echo "Scenario: explicit custom-root opt-in"
custom_root="$TEST_ROOT/custom"
custom_install="$custom_root/opt/pulp/bin"
run_installer "$custom_root" "$custom_install" \
    PULP_ACCEPT_CONTROL_BROKER_CUSTOM_INSTALL_ROOT=1
assert_common_contract "custom install" "$custom_root" "$custom_install"
assert_file_contains "custom install passes broker value plus opt-in" \
    "$custom_root/pulp.log" 'reconcile|argc=10|'
assert_file_contains "custom install passes the hidden custom-root opt-in" \
    "$custom_root/pulp.log" '|arg10=--accept-custom-root|'

echo "Scenario: pinned historical broker-only release"
printf 'historical-runtime\n' > "$TEST_ROOT/payload/libwgpu_native.dylib"
tar -czf "$TEST_ROOT/pulp.tar.gz" -C "$TEST_ROOT/payload" \
    pulp pulp-control-broker libwgpu_native.dylib
historical_root="$TEST_ROOT/historical"
historical_install="$historical_root/home/.pulp/bin"
run_installer "$historical_root" ""
assert_file_contains "historical release uses its broker-only reconciler contract" \
    "$historical_root/pulp.log" 'reconcile|argc=3|'
assert_file_contains "historical release receives the staged broker" \
    "$historical_root/pulp.log" 'contents=broker-fixture'
assert_absent "historical release does not invent a companion host" \
    "$historical_install/pulp-control-standalone-host"
assert_file_contains "historical release retains its shared runtime" \
    "$historical_install/libwgpu_native.dylib" 'historical-runtime'

echo ""
echo "Result: $PASS pass, $FAIL fail"
[[ "$FAIL" == "0" ]]
