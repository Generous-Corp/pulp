#!/usr/bin/env bash
# check-docs.sh — validate docs consistency against the codebase
# Run from project root, or let pulp docs check invoke it.
# Exit 0 if clean, 1 if any issues found.

set -uo pipefail

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$SCRIPT_DIR/.." && pwd)"
DOCS="$ROOT/docs"
STATUS="$DOCS/status"
ERRORS=0
WARNINGS=0

red()    { printf '\033[1;31m%s\033[0m\n' "$1"; }
yellow() { printf '\033[1;33m%s\033[0m\n' "$1"; }
green()  { printf '\033[1;32m%s\033[0m\n' "$1"; }
info()   { printf '  %s\n' "$1"; }

error() { red "ERROR: $1"; ((ERRORS++)); }
warn()  { yellow "WARN:  $1"; ((WARNINGS++)); }

# ── 1. Docs-index completeness ────────────────────────────────────────────────
echo "Checking docs-index completeness..."

# Get all .md files in docs/ (excluding README.md at root)
while IFS= read -r md_file; do
    rel="${md_file#$DOCS/}"
    # Check if this path appears in docs-index.yaml
    if ! grep -q "path: $rel" "$STATUS/docs-index.yaml" 2>/dev/null; then
        warn "docs/$rel not indexed in docs/status/docs-index.yaml"
    fi
done < <(find "$DOCS" -name '*.md' -not -name 'README.md' -not -path '*/assets/*' | sort)

# ── 2. Docs-index paths resolve ──────────────────────────────────────────────
echo "Checking docs-index path resolution..."

while IFS= read -r path_line; do
    # Extract path value from "    path: some/file.md"
    doc_path=$(echo "$path_line" | sed 's/.*path: *//')
    if [ -n "$doc_path" ] && [ ! -f "$DOCS/$doc_path" ]; then
        error "docs-index.yaml references missing file: docs/$doc_path"
    fi
done < <(grep '^ *path:' "$STATUS/docs-index.yaml" 2>/dev/null)

# ── 3. Manifest doc links resolve ────────────────────────────────────────────
echo "Checking manifest doc links..."

for manifest in "$STATUS"/*.yaml; do
    basename=$(basename "$manifest")
    while IFS= read -r docs_line; do
        doc_ref=$(echo "$docs_line" | sed 's/.*docs: *//')
        # Strip anchor (#section)
        doc_file="${doc_ref%%#*}"
        if [ -n "$doc_file" ] && [ ! -f "$DOCS/$doc_file" ]; then
            error "$basename references missing file: docs/$doc_file"
        fi
    done < <(grep '^ *docs:' "$manifest" 2>/dev/null)
done

# ── 4. Status vocabulary enforcement ─────────────────────────────────────────
echo "Checking status vocabulary..."

ALLOWED="stable|usable|experimental|partial|planned|unsupported|active"

for manifest in "$STATUS/support-matrix.yaml" "$STATUS/modules.yaml" "$STATUS/cli-commands.yaml" "$STATUS/cmake-functions.yaml"; do
    [ -f "$manifest" ] || continue
    basename=$(basename "$manifest")
    while IFS= read -r status_line; do
        val=$(echo "$status_line" | sed 's/.*status: *//')
        if ! echo "$val" | grep -qE "^($ALLOWED)$"; then
            error "$basename has invalid status value: '$val'"
        fi
    done < <(grep '^ *status:' "$manifest" 2>/dev/null)
done

# ── 5. Module dependencies: manifest vs CMake ────────────────────────────────
echo "Checking module dependencies against CMake..."

# Parse modules.yaml for each module's declared dependencies
while IFS= read -r line; do
    if echo "$line" | grep -q '^ *- name:'; then
        mod_name=$(echo "$line" | sed 's/.*name: *//')
    fi
    if echo "$line" | grep -q '^ *dependencies:'; then
        declared_deps=$(echo "$line" | sed 's/.*dependencies: *\[//' | sed 's/\]//' | tr ',' '\n' | tr -d ' ' | sort)

        # Find the module's CMakeLists.txt
        cmake_file="$ROOT/core/$mod_name/CMakeLists.txt"
        if [ -f "$cmake_file" ]; then
            # Extract pulp- prefixed link deps, strip the prefix
            cmake_deps=$(grep -oE 'pulp-[a-z]+' "$cmake_file" 2>/dev/null | sed "s/pulp-//" | grep -v "^$mod_name$" | sort -u)

            # Compare: warn if CMake has deps not in manifest
            for dep in $cmake_deps; do
                if [ -n "$dep" ] && ! echo "$declared_deps" | grep -q "^${dep}$"; then
                    warn "module '$mod_name': CMake links pulp-$dep but modules.yaml doesn't list it"
                fi
            done
        fi
    fi
done < "$STATUS/modules.yaml"

# ── 6. Support matrix: format adapters exist ──────────────────────────────────
echo "Checking format adapter source files..."

for pair in "vst3:core/format/vst3" "au_v2:core/format/au" "clap:core/format/clap" "standalone:core/format/standalone"; do
    fmt="${pair%%:*}"
    dir="${pair#*:}"
    # Check if the format is claimed in support-matrix.yaml
    if grep -q "^  $fmt:" "$STATUS/support-matrix.yaml" 2>/dev/null; then
        if [ ! -d "$ROOT/$dir" ]; then
            # Search for source files mentioning this format anywhere in core/format
            if ! find "$ROOT/core/format" -name "*.cpp" -o -name "*.hpp" -o -name "*.h" 2>/dev/null | xargs grep -li "$fmt" >/dev/null 2>&1; then
                warn "support-matrix claims '$fmt' but no adapter found at $dir"
            fi
        fi
    fi
done

# ── 7. Subsystem directories exist ───────────────────────────────────────────
echo "Checking subsystem directories..."

while IFS= read -r line; do
    if echo "$line" | grep -q '^ *- name:'; then
        mod_name=$(echo "$line" | sed 's/.*name: *//')
        if [ ! -d "$ROOT/core/$mod_name" ]; then
            error "modules.yaml lists '$mod_name' but core/$mod_name/ does not exist"
        fi
    fi
done < "$STATUS/modules.yaml"

# ── 8. README test count accuracy ─────────────────────────────────────────────
echo "Checking README accuracy..."

if [ -f "$ROOT/README.md" ] && [ -d "$ROOT/build" ]; then
    # Extract test count from README
    readme_count=$(grep -oE '[0-9]+ automated tests' "$ROOT/README.md" 2>/dev/null | grep -oE '[0-9]+')
    if [ -n "$readme_count" ]; then
        # Get actual test count from build
        actual_count=$(ctest --test-dir "$ROOT/build" -N 2>/dev/null | grep "Total Tests:" | grep -oE '[0-9]+')
        if [ -n "$actual_count" ] && [ "$readme_count" != "$actual_count" ]; then
            warn "README.md says '$readme_count automated tests' but build has $actual_count tests"
        fi
    fi
fi

# ── Summary ───────────────────────────────────────────────────────────────────
echo ""
if [ $ERRORS -gt 0 ]; then
    red "FAILED: $ERRORS error(s), $WARNINGS warning(s)"
    exit 1
elif [ $WARNINGS -gt 0 ]; then
    yellow "PASSED with $WARNINGS warning(s)"
    exit 0
else
    green "PASSED: all docs checks clean"
    exit 0
fi
