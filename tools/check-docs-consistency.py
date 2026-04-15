#!/usr/bin/env python3
"""
Cross-check docs/status/support-matrix.yaml against docs/reference/capabilities.md.

Pure-Python (no PyYAML dependency) — uses targeted regex extraction rather than
a full YAML parse, which is sufficient for the drift checks we need today.

Rules enforced:
1. Every `status:` value in support-matrix.yaml uses the allowed vocabulary.
2. For platform_maturity.accessibility, every platform listed (macos/ios/android/
   windows/linux) has a matching row in the capabilities.md "Platform Maturity"
   table with the same status label.

Exit codes:
  0 — consistent
  1 — drift found
  2 — input error
"""
from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[1]
MATRIX_PATH = REPO_ROOT / "docs" / "status" / "support-matrix.yaml"
CAPABILITIES_PATH = REPO_ROOT / "docs" / "reference" / "capabilities.md"

ALLOWED_STATUSES = {
    "stable", "usable", "experimental", "partial", "planned", "unsupported",
}

# (platform_key_in_matrix, capability_label_in_capabilities_md,
#  platform_label_in_capabilities_md)
ACCESSIBILITY_ROWS = [
    ("macos",   "VoiceOver accessibility", "macOS"),
    ("ios",     "VoiceOver accessibility", "iOS"),
    ("android", "TalkBack accessibility",  "Android"),
    ("windows", "UIA accessibility",       "Windows"),
    ("linux",   "AT-SPI accessibility",    "Linux"),
]


def extract_matrix_accessibility(text: str) -> dict[str, str]:
    """Return platform -> status for platform_maturity.accessibility entries."""
    # Find the accessibility block and read up until the next top-level key
    # at the same indent as `accessibility:`.
    m = re.search(
        r"^(\s*)accessibility:\s*\n(.*?)(?=^\1\S|\Z)",
        text,
        flags=re.MULTILINE | re.DOTALL,
    )
    if not m:
        return {}
    block = m.group(2)
    result: dict[str, str] = {}
    for plat_match in re.finditer(
        r"^\s+([a-z]+):\s*\n\s+status:\s*(\S+)",
        block,
        flags=re.MULTILINE,
    ):
        platform, status = plat_match.group(1), plat_match.group(2)
        result[platform] = status.strip('"').strip("'")
    return result


def extract_all_statuses(text: str) -> list[tuple[int, str]]:
    """Return (line_no, status_value) for every `status:` line."""
    out = []
    for i, line in enumerate(text.splitlines(), start=1):
        m = re.match(r"\s*status:\s*(\S+)", line)
        if m:
            out.append((i, m.group(1).strip('"').strip("'")))
    return out


def parse_platform_maturity_rows(md_text: str) -> list[tuple[str, str, str]]:
    """Return (capability, status, platform) tuples from the Platform Maturity table."""
    m = re.search(
        r"### Platform Maturity\s*\n\n"
        r"\| Capability \| Status \| Platform \| Notes \|\s*\n"
        r"\|[^\n]*\n"
        r"((?:\|[^\n]*\n)+)",
        md_text,
    )
    if not m:
        return []
    rows = []
    for line in m.group(1).strip().splitlines():
        parts = [p.strip() for p in line.strip().strip("|").split("|")]
        if len(parts) < 4:
            continue
        capability, status, platform, _notes = parts[:4]
        rows.append((capability, status, platform))
    return rows


def main() -> int:
    try:
        matrix_text = MATRIX_PATH.read_text()
    except Exception as e:
        sys.stderr.write(f"Failed to read {MATRIX_PATH}: {e}\n")
        return 2
    try:
        md_text = CAPABILITIES_PATH.read_text()
    except Exception as e:
        sys.stderr.write(f"Failed to read {CAPABILITIES_PATH}: {e}\n")
        return 2

    problems: list[str] = []

    # 1. Status vocabulary
    for line_no, status in extract_all_statuses(matrix_text):
        if status not in ALLOWED_STATUSES:
            problems.append(
                f"support-matrix.yaml:{line_no}: invalid status '{status}'"
            )

    # 2. Accessibility row cross-check
    matrix_a11y = extract_matrix_accessibility(matrix_text)
    md_rows = parse_platform_maturity_rows(md_text)
    md_lookup = {(cap, plat): status for cap, status, plat in md_rows}

    for platform_key, cap_label, md_plat_label in ACCESSIBILITY_ROWS:
        matrix_status = matrix_a11y.get(platform_key)
        if matrix_status is None:
            problems.append(
                f"support-matrix.yaml: platform_maturity.accessibility.{platform_key} missing"
            )
            continue
        md_status = md_lookup.get((cap_label, md_plat_label))
        if md_status is None:
            problems.append(
                f"capabilities.md: Platform Maturity row '{cap_label}' "
                f"for platform '{md_plat_label}' missing"
            )
            continue
        if md_status != matrix_status:
            problems.append(
                f"drift: '{cap_label}' ({md_plat_label}) "
                f"matrix={matrix_status} capabilities.md={md_status}"
            )

    if problems:
        print("docs-consistency: DRIFT DETECTED")
        for p in problems:
            print(f"  - {p}")
        return 1
    print("docs-consistency: OK")
    print(f"  platform_maturity.accessibility platforms checked: {len(ACCESSIBILITY_ROWS)}")
    print(f"  status values scanned: {len(extract_all_statuses(matrix_text))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
