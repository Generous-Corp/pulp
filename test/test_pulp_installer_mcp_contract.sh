#!/usr/bin/env bash
# Hermetic contract coverage for the public CLI installers' MCP wiring.
#
# The Unix scenarios execute the canonical installer through `sh`, matching the
# documented curl pipeline. Network and archive operations are replaced with
# PATH shims, so no release assets or user profiles are touched.

set -euo pipefail

PULP_SRC="$(cd "$(dirname "$0")/.." && pwd)"
INSTALL_SH="$PULP_SRC/tools/install/install.sh"
INSTALL_PS1="$PULP_SRC/tools/install/install.ps1"
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
    if [[ -f "$file" ]] && grep -qF "$expected" "$file"; then
        pass "$label"
    else
        fail "$label (missing '$expected' in $file)"
    fi
}

assert_file_excludes() {
    local label="$1" file="$2" unexpected="$3"
    if [[ ! -f "$file" ]] || ! grep -qF "$unexpected" "$file"; then
        pass "$label"
    else
        fail "$label (unexpected '$unexpected' in $file)"
    fi
}

assert_file_count() {
    local label="$1" file="$2" expected="$3" count="$4"
    local actual
    actual=$(grep -cF "$expected" "$file" 2>/dev/null || true)
    if [[ "$actual" == "$count" ]]; then
        pass "$label"
    else
        fail "$label (expected $count occurrences of '$expected', found $actual)"
    fi
}

make_mock_commands() {
    local mock_bin="$1"
    mkdir -p "$mock_bin"

    cat > "$mock_bin/uname" <<'EOF'
#!/bin/sh
case "${1:-}" in
    -s) echo Linux ;;
    -m) echo x86_64 ;;
    *) echo Linux ;;
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
if [ -n "$output" ]; then
    : > "$output"
fi
EOF

    cat > "$mock_bin/tar" <<'EOF'
#!/bin/sh
destination=
while [ "$#" -gt 0 ]; do
    if [ "$1" = "-C" ]; then
        shift
        destination="$1"
    fi
    shift
done
[ -n "$destination" ] || exit 2
mkdir -p "$destination"
cat > "$destination/pulp" <<'PULP_EOF'
#!/bin/sh
case "${1:-}" in
    --version) echo "0.0.0-test" ;;
    sdk)
        case "${2:-}" in
            install) exit 0 ;;
            status) echo "0.0.0-test (downloaded)" ;;
        esac
        ;;
esac
PULP_EOF
chmod +x "$destination/pulp"
if [ "${MOCK_INCLUDE_MCP:-1}" = "1" ]; then
    cat > "$destination/pulp-mcp" <<'MCP_EOF'
#!/bin/sh
echo "pulp-mcp 0.0.0-test"
MCP_EOF
    chmod +x "$destination/pulp-mcp"
fi
EOF

    chmod +x "$mock_bin/uname" "$mock_bin/curl" "$mock_bin/tar"
}

run_installer() {
    local case_root="$1" shell_path="$2" install_dir="$3"
    shift 3
    (
        cd "$case_root"
        env \
            HOME="$case_root/home" \
            SHELL="$shell_path" \
            PATH="$case_root/mock-bin:$PATH" \
            PULP_VERSION="0.0.0-test" \
            PULP_SKIP_SDK_INSTALL=1 \
            PULP_INSTALL_DIR="$install_dir" \
            "$@" \
            sh "$INSTALL_SH"
    )
}

[[ -f "$INSTALL_SH" ]] || { echo "FAIL: missing $INSTALL_SH"; exit 2; }
[[ -f "$INSTALL_PS1" ]] || { echo "FAIL: missing $INSTALL_PS1"; exit 2; }

echo "Scenario: PATH already present, relative custom path, managed upgrade"
managed_root="$TEST_ROOT/managed"
mkdir -p "$managed_root/home"
make_mock_commands "$managed_root/mock-bin"
managed_install="$managed_root/custom install/bin"
cat > "$managed_root/home/.zshrc" <<'EOF'
export PATH="/already/present:$PATH"
# >>> Pulp MCP (managed by installer) >>>
export PULP_MCP_BINARY="/stale/pulp-mcp"
# <<< Pulp MCP (managed by installer) <<<
EOF
managed_output="$managed_root/output.txt"
(
    cd "$managed_root"
    env \
        HOME="$managed_root/home" \
        SHELL=/bin/zsh \
        PATH="$managed_root/mock-bin:$managed_install:$PATH" \
        PULP_VERSION=0.0.0-test \
        PULP_SKIP_SDK_INSTALL=1 \
        PULP_INSTALL_DIR="custom install/bin" \
        sh "$INSTALL_SH"
) >"$managed_output" 2>&1
assert_file_contains \
    "relative custom install path is normalized in PATH assignment" \
    "$managed_root/home/.zshrc" \
    "export PATH='$managed_install':\$PATH"
assert_file_contains \
    "managed MCP assignment is updated to normalized custom path" \
    "$managed_root/home/.zshrc" \
    "export PULP_MCP_BINARY='$managed_install/pulp-mcp'"
assert_file_excludes \
    "managed stale MCP path is removed" \
    "$managed_root/home/.zshrc" \
    "/stale/pulp-mcp"
if [[ -x "$managed_install/pulp-mcp" ]]; then
    pass "mock archive installed MCP binary under normalized path"
else
    fail "mock archive did not install MCP binary under normalized path"
fi

echo "Scenario: user-managed MCP override is preserved"
override_root="$TEST_ROOT/override"
mkdir -p "$override_root/home"
make_mock_commands "$override_root/mock-bin"
cat > "$override_root/home/.bashrc" <<'EOF'
export PULP_MCP_BINARY="/user/source-build/pulp-mcp"
EOF
cat > "$override_root/home/.bash_profile" <<'EOF'
. "$HOME/.bashrc"
EOF
run_installer "$override_root" /bin/bash "$override_root/bin" \
    >"$override_root/output.txt" 2>&1
assert_file_contains \
    "user-managed MCP assignment remains unchanged" \
    "$override_root/home/.bashrc" \
    'export PULP_MCP_BINARY="/user/source-build/pulp-mcp"'
assert_file_excludes \
    "installer does not add a managed block over user override" \
    "$override_root/home/.bashrc" \
    "# >>> Pulp MCP (managed by installer) >>>"
assert_file_contains \
    "installer reports preserved user override" \
    "$override_root/output.txt" \
    "Preserved user-managed PULP_MCP_BINARY"
assert_file_contains \
    "login profile receives a non-overriding MCP fallback" \
    "$override_root/home/.bash_profile" \
    "if [ -z \"\${PULP_MCP_BINARY+x}\" ]; then export PULP_MCP_BINARY='$override_root/bin/pulp-mcp'; fi"
resolved_override=$(
    env -u PULP_MCP_BINARY HOME="$override_root/home" \
        bash -c '. "$HOME/.bash_profile"; printf "%s" "$PULP_MCP_BINARY"'
)
if [[ "$resolved_override" == "/user/source-build/pulp-mcp" ]]; then
    pass "login profile preserves the override sourced from .bashrc"
else
    fail "login profile resolved MCP binary to '$resolved_override'"
fi

echo "Scenario: existing PATH syntax and ordering are preserved"
path_root="$TEST_ROOT/path-profile"
mkdir -p "$path_root/home"
make_mock_commands "$path_root/mock-bin"
cat > "$path_root/home/.zshrc" <<EOF
export PATH="\$PATH:$path_root/bin"
EOF
run_installer "$path_root" /bin/zsh "$path_root/bin" \
    >"$path_root/output.txt" 2>&1
assert_file_count \
    "existing PATH assignment is not duplicated" \
    "$path_root/home/.zshrc" \
    "export PATH=" \
    1
assert_file_contains \
    "user-selected PATH append ordering remains intact" \
    "$path_root/home/.zshrc" \
    "export PATH=\"\$PATH:$path_root/bin\""
assert_file_excludes \
    "installer does not report adding an equivalent PATH assignment" \
    "$path_root/output.txt" \
    "Added $path_root/bin to PATH"

echo "Scenario: comments, docs, and unset mentions do not masquerade as assignments"
mention_root="$TEST_ROOT/text-mentions"
mkdir -p "$mention_root/home"
make_mock_commands "$mention_root/mock-bin"
cat > "$mention_root/home/.zshrc" <<'EOF'
# export PULP_MCP_BINARY=/commented
unset PULP_MCP_BINARY
echo "Set PULP_MCP_BINARY when using a source build" >/dev/null
set OTHER PULP_MCP_BINARY
EOF
run_installer "$mention_root" /bin/zsh "$mention_root/bin" \
    >"$mention_root/output.txt" 2>&1
assert_file_contains \
    "textual mentions still receive managed MCP assignment" \
    "$mention_root/home/.zshrc" \
    "export PULP_MCP_BINARY='$mention_root/bin/pulp-mcp'"
assert_file_excludes \
    "textual mentions are not reported as a preserved override" \
    "$mention_root/output.txt" \
    "Preserved user-managed PULP_MCP_BINARY"

echo "Scenario: Bash login and non-login profiles receive one managed assignment"
bash_root="$TEST_ROOT/bash-profiles"
mkdir -p "$bash_root/home"
make_mock_commands "$bash_root/mock-bin"
printf '%s\n' "# login" > "$bash_root/home/.bash_profile"
printf '%s\n' "# non-login" > "$bash_root/home/.bashrc"
run_installer "$bash_root" /bin/bash "$bash_root/bin" \
    >"$bash_root/first-output.txt" 2>&1
run_installer "$bash_root" /bin/bash "$bash_root/bin" \
    >"$bash_root/second-output.txt" 2>&1
for bash_profile in "$bash_root/home/.bash_profile" "$bash_root/home/.bashrc"; do
    assert_file_contains \
        "Bash profile receives MCP assignment: $bash_profile" \
        "$bash_profile" \
        "if [ -z \"\${PULP_MCP_BINARY+x}\" ]; then export PULP_MCP_BINARY='$bash_root/bin/pulp-mcp'; fi"
    assert_file_count \
        "Bash profile keeps exactly one managed start marker: $bash_profile" \
        "$bash_profile" \
        "# >>> Pulp MCP (managed by installer) >>>" \
        1
    assert_file_count \
        "Bash profile keeps exactly one managed end marker: $bash_profile" \
        "$bash_profile" \
        "# <<< Pulp MCP (managed by installer) <<<" \
        1
done

echo "Scenario: Bash without .bash_profile configures login and non-login profiles"
bash_fallback_root="$TEST_ROOT/bash-profile-fallback"
mkdir -p "$bash_fallback_root/home"
make_mock_commands "$bash_fallback_root/mock-bin"
run_installer "$bash_fallback_root" /bin/bash "$bash_fallback_root/bin" \
    >"$bash_fallback_root/output.txt" 2>&1
for bash_profile in "$bash_fallback_root/home/.bashrc" "$bash_fallback_root/home/.profile"; do
    assert_file_contains \
        "Bash fallback profile receives MCP assignment: $bash_profile" \
        "$bash_profile" \
        "if [ -z \"\${PULP_MCP_BINARY+x}\" ]; then export PULP_MCP_BINARY='$bash_fallback_root/bin/pulp-mcp'; fi"
done
if [[ ! -e "$bash_fallback_root/home/.bash_profile" ]]; then
    pass "Bash fallback does not invent .bash_profile"
else
    fail "Bash fallback invented .bash_profile"
fi

echo "Scenario: malformed marker shapes are preserved byte-for-byte"
for malformed_kind in missing-end reversed duplicate; do
    malformed_root="$TEST_ROOT/malformed-$malformed_kind"
    mkdir -p "$malformed_root/home"
    make_mock_commands "$malformed_root/mock-bin"
    if [[ "$malformed_kind" == "missing-end" ]]; then
        cat > "$malformed_root/home/.zshrc" <<'EOF'
before
# >>> Pulp MCP (managed by installer) >>>
export PULP_MCP_BINARY="/old/pulp-mcp"
tail-must-survive
EOF
    elif [[ "$malformed_kind" == "reversed" ]]; then
        cat > "$malformed_root/home/.zshrc" <<'EOF'
before
# <<< Pulp MCP (managed by installer) <<<
export PULP_MCP_BINARY="/old/pulp-mcp"
# >>> Pulp MCP (managed by installer) >>>
tail-must-survive
EOF
    else
        cat > "$malformed_root/home/.zshrc" <<'EOF'
before
# >>> Pulp MCP (managed by installer) >>>
# >>> Pulp MCP (managed by installer) >>>
export PULP_MCP_BINARY="/old/pulp-mcp"
# <<< Pulp MCP (managed by installer) <<<
tail-must-survive
EOF
    fi
    cp "$malformed_root/home/.zshrc" "$malformed_root/profile.before"
    run_installer "$malformed_root" /bin/zsh "$malformed_root/bin" \
        >"$malformed_root/output.txt" 2>&1
    original_lines=$(wc -l < "$malformed_root/profile.before")
    head -n "$original_lines" "$malformed_root/home/.zshrc" \
        > "$malformed_root/profile.after-prefix"
    if cmp -s "$malformed_root/profile.before" "$malformed_root/profile.after-prefix"; then
        pass "$malformed_kind managed block and profile tail stay byte-identical"
    else
        fail "$malformed_kind managed markers changed existing profile content"
    fi
    assert_file_contains \
        "$malformed_kind managed markers produce repair diagnostic" \
        "$malformed_root/output.txt" \
        "Preserved malformed Pulp MCP block"
done

echo "Scenario: managed MCP update preserves a symlinked profile"
symlink_root="$TEST_ROOT/symlink"
mkdir -p "$symlink_root/home/dotfiles"
make_mock_commands "$symlink_root/mock-bin"
cat > "$symlink_root/home/dotfiles/zshrc" <<'EOF'
# >>> Pulp MCP (managed by installer) >>>
export PULP_MCP_BINARY="/stale/pulp-mcp"
# <<< Pulp MCP (managed by installer) <<<
tail-must-survive
EOF
ln -s "dotfiles/zshrc" "$symlink_root/home/.zshrc"
run_installer "$symlink_root" /bin/zsh "$symlink_root/bin" \
    >"$symlink_root/output.txt" 2>&1
if [[ -L "$symlink_root/home/.zshrc" ]]; then
    pass "managed update preserves the profile symlink"
else
    fail "managed update replaced the profile symlink"
fi
assert_file_contains \
    "managed update writes through to symlink target" \
    "$symlink_root/home/dotfiles/zshrc" \
    "export PULP_MCP_BINARY='$symlink_root/bin/pulp-mcp'"
assert_file_contains \
    "managed symlink update preserves profile tail" \
    "$symlink_root/home/dotfiles/zshrc" \
    "tail-must-survive"

echo "Scenario: archive without pulp-mcp does not configure MCP"
absent_root="$TEST_ROOT/absent"
mkdir -p "$absent_root/home"
make_mock_commands "$absent_root/mock-bin"
run_installer "$absent_root" /bin/zsh "$absent_root/bin" \
    MOCK_INCLUDE_MCP=0 >"$absent_root/output.txt" 2>&1
assert_file_excludes \
    "missing MCP binary produces no profile assignment" \
    "$absent_root/home/.zshrc" \
    "PULP_MCP_BINARY"
assert_file_contains \
    "missing MCP binary still configures CLI PATH" \
    "$absent_root/home/.zshrc" \
    "export PATH='$absent_root/bin':\$PATH"

echo "Scenario: Fish assignments quote a custom path containing spaces"
fish_root="$TEST_ROOT/fish"
mkdir -p "$fish_root/home"
make_mock_commands "$fish_root/mock-bin"
fish_install="$fish_root/custom install/bin"
run_installer "$fish_root" /usr/bin/fish "$fish_install" \
    >"$fish_root/output.txt" 2>&1
fish_profile="$fish_root/home/.config/fish/config.fish"
assert_file_contains \
    "Fish PATH assignment keeps custom directory as one element" \
    "$fish_profile" \
    "set -gx PATH '$fish_install' \$PATH"
assert_file_contains \
    "Fish MCP assignment quotes custom executable path" \
    "$fish_profile" \
    "set -gx PULP_MCP_BINARY '$fish_install/pulp-mcp'"

echo "Scenario: shell metacharacters round-trip in custom install paths"
special_root="$TEST_ROOT/special-path"
mkdir -p "$special_root/home"
make_mock_commands "$special_root/mock-bin"
special_component="custom \$cash \`tick\` \"quote\" \\slash 'single"
special_install="$special_root/$special_component/bin"
run_installer "$special_root" /bin/zsh "$special_install" \
    >"$special_root/output.txt" 2>&1
if env -i PATH=/usr/bin:/bin sh -c \
    '. "$1"; [ "$PULP_MCP_BINARY" = "$2" ]' \
    sh "$special_root/home/.zshrc" "$special_install/pulp-mcp"; then
    pass "POSIX profile assignment round-trips shell metacharacters"
else
    fail "POSIX profile assignment changed shell metacharacters"
fi

fish_special_root="$TEST_ROOT/fish-special-path"
mkdir -p "$fish_special_root/home"
make_mock_commands "$fish_special_root/mock-bin"
fish_special_install="$fish_special_root/$special_component/bin"
run_installer "$fish_special_root" /usr/bin/fish "$fish_special_install" \
    >"$fish_special_root/output.txt" 2>&1
fish_special_profile="$fish_special_root/home/.config/fish/config.fish"
fish_special_escaped=$(printf '%s' "$fish_special_install/pulp-mcp" |
    sed -e 's/\\/\\\\/g' -e "s/'/\\\\'/g")
assert_file_contains \
    "Fish MCP assignment safely escapes shell metacharacters" \
    "$fish_special_profile" \
    "set -gx PULP_MCP_BINARY '$fish_special_escaped'"

echo "Scenario: PULP_NO_MODIFY_PATH leaves profile untouched"
no_modify_root="$TEST_ROOT/no-modify"
mkdir -p "$no_modify_root/home"
make_mock_commands "$no_modify_root/mock-bin"
printf '%s\n' "# existing profile" > "$no_modify_root/home/.zshrc"
cp "$no_modify_root/home/.zshrc" "$no_modify_root/profile.before"
run_installer "$no_modify_root" /bin/zsh "$no_modify_root/bin" \
    PULP_NO_MODIFY_PATH=1 >"$no_modify_root/output.txt" 2>&1
if cmp -s "$no_modify_root/profile.before" "$no_modify_root/home/.zshrc"; then
    pass "PULP_NO_MODIFY_PATH leaves shell profile byte-identical"
else
    fail "PULP_NO_MODIFY_PATH modified shell profile"
fi
assert_file_contains \
    "no-modify output gives manual MCP export" \
    "$no_modify_root/output.txt" \
    "export PULP_MCP_BINARY='$no_modify_root/bin/pulp-mcp'"

echo "Scenario: canonical PowerShell installer persists full .exe user path"
assert_file_contains \
    "PowerShell normalizes custom install directory" \
    "$INSTALL_PS1" \
    '$InstallDir = [System.IO.Path]::GetFullPath($InstallDir)'
assert_file_contains \
    "PowerShell constructs pulp-mcp.exe beneath install directory" \
    "$INSTALL_PS1" \
    '$PulpMcpExe = Join-Path $InstallDir "pulp-mcp.exe"'
assert_file_contains \
    "PowerShell persists MCP executable in user environment" \
    "$INSTALL_PS1" \
    '[Environment]::SetEnvironmentVariable("PULP_MCP_BINARY", $PulpMcpExe, "User")'
assert_file_contains \
    "PowerShell updates current process environment" \
    "$INSTALL_PS1" \
    '$env:PULP_MCP_BINARY = $PulpMcpExe'
assert_file_contains \
    "PowerShell records installer ownership outside public environment" \
    "$INSTALL_PS1" \
    '[Microsoft.Win32.Registry]::CurrentUser.CreateSubKey("Software\Pulp")'
assert_file_contains \
    "PowerShell ownership marker names the managed MCP path" \
    "$INSTALL_PS1" \
    '"ManagedPulpMcpBinary"'
assert_file_contains \
    "PowerShell preserves an unowned or changed user override" \
    "$INSTALL_PS1" \
    'Write-Host "Preserved user-managed PULP_MCP_BINARY=$CurrentUserMcp"'
assert_file_contains \
    "PowerShell honors no-modify contract before persistence" \
    "$INSTALL_PS1" \
    'if ((Test-Path $PulpMcpExe) -and (-not $NoModifyPath))'

echo ""
echo "Result: $PASS pass, $FAIL fail"
[[ "$FAIL" -eq 0 ]]
