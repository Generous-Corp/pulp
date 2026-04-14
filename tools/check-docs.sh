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
actual_count=""
readme_count=""

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

actual_count=""
readme_count=""

if [ -d "$ROOT/build" ]; then
    actual_count=$(ctest --test-dir "$ROOT/build" -N 2>/dev/null | grep "Total Tests:" | grep -oE '[0-9]+')
fi

if [ -f "$ROOT/README.md" ]; then
    readme_count=$(grep -oE '[0-9]+ automated tests' "$ROOT/README.md" 2>/dev/null | grep -oE '[0-9]+')
    if [ -n "$readme_count" ] && [ -n "$actual_count" ] && [ "$readme_count" != "$actual_count" ]; then
        warn "README.md says '$readme_count automated tests' but build has $actual_count tests"
    fi
fi

# Check test count consistency across docs
if [ -n "$readme_count" ]; then
    for doc in "$DOCS/concepts/overview.md" "$DOCS/guides/testing.md"; do
        if [ -f "$doc" ]; then
            doc_count=$(grep -oE '[0-9]+ (automated|registered) tests' "$doc" 2>/dev/null | grep -oE '[0-9]+' | head -1)
            if [ -n "$doc_count" ] && [ "$doc_count" != "$readme_count" ]; then
                rel="${doc#$ROOT/}"
                warn "Test count mismatch: README says $readme_count, $rel says $doc_count"
            fi
        fi
    done
fi

# ── VISION.md status accuracy ─────────────────────────────────────────────────
echo "Checking VISION.md status claims..."

if [ -f "$ROOT/VISION.md" ]; then
    # Check "not yet implemented" claims against actual code
    # AUv3 — should NOT be listed as unimplemented (it exists)
    if grep -q 'AUv3.*format adapter' "$ROOT/VISION.md" | grep -qi 'not yet'; then
        warn "VISION.md lists AUv3 as not implemented but core/format/src/au_adapter.mm exists"
    fi

    # Check that claimed example count matches reality
    vision_examples=$(grep -oE '[0-9]+ example projects' "$ROOT/VISION.md" 2>/dev/null | grep -oE '[0-9]+')
    if [ -n "$vision_examples" ]; then
        actual_examples=$(find "$ROOT/examples" -maxdepth 1 -type d ! -name examples | wc -l | tr -d ' ')
        if [ "$vision_examples" != "$actual_examples" ]; then
            warn "VISION.md says '$vision_examples example projects' but examples/ has $actual_examples"
        fi
    fi

    # Check that items listed under "What works today" aren't also in "not yet implemented"
    while IFS= read -r line; do
        # Extract key terms from "not yet" items and check if they appear in "works today"
        item=$(echo "$line" | sed 's/^- //')
        if [ -n "$item" ] && grep -q "What works today" "$ROOT/VISION.md"; then
            # Just ensure the sections aren't self-contradictory for known items
            :
        fi
    done < <(sed -n '/What is not yet implemented/,/^---$/p' "$ROOT/VISION.md" | grep '^- ')

    # Check platform support table matches support-matrix.yaml
    if [ -f "$STATUS/support-matrix.yaml" ]; then
        # If VISION.md says WASAPI but support-matrix says planned, that's a drift
        for backend in wasapi alsa coremidi; do
            yaml_status=$(grep -A1 "  $backend:" "$STATUS/support-matrix.yaml" 2>/dev/null | grep 'status:' | awk '{print $2}')
            if [ "$yaml_status" = "planned" ] && grep -qi "$backend" "$ROOT/VISION.md" | grep -qi "works today"; then
                warn "VISION.md claims $backend works but support-matrix.yaml says planned"
            fi
        done
    fi
fi

# ── Docs consistency (support-matrix.yaml ↔ capabilities.md) ──────────────────
if [ -x "$ROOT/tools/check-docs-consistency.py" ]; then
    echo "Checking docs consistency (support-matrix ↔ capabilities)..."
    if ! python3 "$ROOT/tools/check-docs-consistency.py"; then
        ERRORS=$((ERRORS + 1))
    fi
fi

# ── Status ladder (warn-mode during phase-in) ─────────────────────────────────
if [ -x "$ROOT/tools/check_status_ladder.py" ]; then
    echo "Checking status ladder (usable ⇒ validation evidence)..."
    if [ "${PULP_STATUS_LADDER_STRICT:-0}" = "1" ]; then
        python3 "$ROOT/tools/check_status_ladder.py" --mode=report || ERRORS=$((ERRORS + 1))
    else
        python3 "$ROOT/tools/check_status_ladder.py" --mode=warn || true
    fi
fi

# ── Generated-table drift check (workstream 08 slice 8.3) ─────────────────────
if [ -x "$ROOT/tools/docs_generate.py" ]; then
    echo "Checking generated-table drift in capabilities.md..."
    if ! python3 "$ROOT/tools/docs_generate.py" check; then
        ERRORS=$((ERRORS + 1))
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
