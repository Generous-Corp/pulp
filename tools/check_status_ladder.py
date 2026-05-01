#!/usr/bin/env python3
"""
Enforce the Pulp status ladder on docs/status/support-matrix.yaml.

Ladder rule (production-readiness workstream 08 sub-deliverable 8.4):
A capability may be labeled `usable` only if it has ONE OF:
  (a) cross-platform validation — evidenced by either being nested under a
      platform-scoped parent (e.g. `platform_maturity.accessibility.macos`)
      or having an explicit `platform:` field, OR
  (b) evidence in its `notes:` that validation exists — keyword match against
      a small allow-list ("test", "tests", "validated", "validator", "ci",
      "golden", "auval", "pluginval", "clap-validator", "lv2lint"), OR
  (c) an explicit waiver in `.status-ladder-waivers.txt` at the repo root,
      one YAML path per line (e.g. `plugin_formats.clap.editor`).

Modes:
  --mode=warn     print findings, exit 0 (default; suitable for hints)
  --mode=report   print findings, exit 1 if any (CI gate)

Exit codes:
  0 — clean, or warn mode
  1 — violations found in report mode
  2 — input error
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
MATRIX_PATH = REPO_ROOT / "docs" / "status" / "support-matrix.yaml"
WAIVERS_PATH = REPO_ROOT / ".status-ladder-waivers.txt"

VALIDATION_KEYWORDS = re.compile(
    r"\b("
    r"test|tests|validat\w*|"
    r"ci\b|"
    r"golden|"
    r"auval|pluginval|clap-validator|lv2lint|"
    r"round[- ]?trip|headless|screenshot"
    r")\b",
    re.IGNORECASE,
)

# A platform-scoped parent key (entries nested under these are considered
# already platform-scoped and therefore pass ladder rule (a)).
PLATFORM_KEYS = {"macos", "ios", "android", "windows", "linux", "web", "wasm"}


def load_waivers() -> set[str]:
    if not WAIVERS_PATH.exists():
        return set()
    waivers = set()
    for line in WAIVERS_PATH.read_text().splitlines():
        line = line.split("#", 1)[0].strip()
        if line:
            waivers.add(line)
    return waivers


def walk_statuses(matrix_text: str):
    """
    Yield (path, status, notes_text, is_platform_scoped) for every `status:` line
    in the matrix. Indentation-based parser; matches the matrix's shape.
    """
    lines = matrix_text.splitlines()
    # Track path stack as [(indent_spaces, key)]
    stack: list[tuple[int, str]] = []

    i = 0
    while i < len(lines):
        line = lines[i]
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            i += 1
            continue

        indent = len(line) - len(line.lstrip(" "))

        # Pop stack until we're at a shallower indent than current
        while stack and stack[-1][0] >= indent:
            stack.pop()

        # Key line "foo:" (possibly with trailing value)
        m_key = re.match(r"^(\s*)([A-Za-z0-9_\-]+):\s*(.*)$", line)
        if not m_key:
            i += 1
            continue
        key = m_key.group(2)
        value = m_key.group(3).strip()

        if key == "status":
            path = ".".join(k for _, k in stack)
            is_platform_scoped = any(
                k in PLATFORM_KEYS for _, k in stack
            )

            # Scan following sibling lines within this entry for notes:
            notes = ""
            entry_indent = stack[-1][0] if stack else -1
            j = i + 1
            while j < len(lines):
                next_line = lines[j]
                if not next_line.strip():
                    j += 1
                    continue
                next_indent = len(next_line) - len(next_line.lstrip(" "))
                if next_indent <= entry_indent:
                    break
                m_notes = re.match(r"^\s*notes:\s*(.*)$", next_line)
                if m_notes:
                    notes_val = m_notes.group(1).strip()
                    if notes_val in (">", "|", ">-", "|-"):
                        # Block scalar — read continuation lines
                        k = j + 1
                        buf = []
                        while k < len(lines):
                            cl = lines[k]
                            if not cl.strip():
                                buf.append("")
                                k += 1
                                continue
                            c_indent = len(cl) - len(cl.lstrip(" "))
                            if c_indent <= next_indent:
                                break
                            buf.append(cl.strip())
                            k += 1
                        notes = " ".join(buf).strip()
                    else:
                        notes = notes_val
                    break
                j += 1

            yield (path, value.strip('"').strip("'"), notes, is_platform_scoped)

        stack.append((indent, key))
        i += 1


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("--mode", choices=("warn", "report"), default="warn")
    args = ap.parse_args()

    try:
        matrix_text = MATRIX_PATH.read_text()
    except Exception as e:
        sys.stderr.write(f"Failed to read {MATRIX_PATH}: {e}\n")
        return 2

    waivers = load_waivers()

    violations: list[tuple[str, str]] = []
    checked = 0
    waived = 0

    for path, status, notes, is_platform_scoped in walk_statuses(matrix_text):
        if status != "usable":
            continue
        checked += 1
        if is_platform_scoped:
            continue
        if path in waivers:
            waived += 1
            continue
        if VALIDATION_KEYWORDS.search(notes or ""):
            continue
        violations.append((path, notes))

    print(
        f"status-ladder: checked {checked} `usable` entries, "
        f"{waived} waived, {len(violations)} violations (mode={args.mode})"
    )
    for path, notes in violations:
        short = (notes[:80] + "…") if notes and len(notes) > 80 else notes
        print(f"  - {path}: no validation evidence in notes → '{short}'")

    if violations and args.mode == "report":
        print()
        print("To waive an entry, add its path to .status-ladder-waivers.txt")
        print("To remove the violation, add validation language to notes: (test, "
              "validated, auval, pluginval, clap-validator, ci, golden, etc.)")
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
