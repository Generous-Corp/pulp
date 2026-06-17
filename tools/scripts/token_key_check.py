#!/usr/bin/env python3
"""token_key_check.py — guard against the silent wrong-token-key bug class.

A widget that calls resolve_color("typo.key", <hardcoded fallback>) where
"typo.key" is not a real theme token compiles fine, renders the hardcoded
fallback, and silently breaks reskinning (the token swap never reaches it).
This is exactly what shipped the coral ProgressBar / Tab underline and the
CallOutBox/ListBox/key-mapping greys.

This check extracts the canonical color-token set (every `t.colors["..."]`
assigned anywhere in theme_presets.cpp — derive_theme + all preset overrides)
and flags any `resolve_color("dotted.key", ...)` in the view paint code whose
key is neither canonical nor an explicitly-allowlisted widget-specific override
token. Pure stdlib — runs on every CI lane (unlike the Pillow visual gate).
"""
from __future__ import annotations

import os
import re
import sys

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
PRESETS = os.path.join(REPO, "core/view/src/theme_presets.cpp")
SCAN_DIRS = [
    "core/view/src",
    "core/view/src/widgets",
]

# Widget-specific override tokens that intentionally are NOT in derive_theme:
# they let a theme distinctly skin one widget, and ALWAYS fall back to a
# canonical token (resolve_color(override, resolve_color(canonical, ...))), so a
# missing override is harmless. Keep this list tiny and justified.
ALLOWLISTED_OVERRIDES = {
    "text_editor_bg",        # TextEditor: distinct field bg; falls back to bg.surface
    "text_editor_focus_bg",  # TextEditor: distinct focus bg; falls back to bg.elevated
    "border",                # TextEditor paint: hairline field border; falls back to a literal
    "selection.bg",          # Table: selected-row background; falls back to a literal accent-blue
}

KEY_RE = re.compile(r'resolve_color\(\s*"([a-z][a-zA-Z0-9_.]*)"')


def canonical_keys() -> set[str]:
    with open(PRESETS, encoding="utf-8") as f:
        src = f.read()
    return set(re.findall(r't\.colors\["([^"]+)"\]', src))


def main() -> int:
    canon = canonical_keys()
    if not canon:
        print(f"FAIL: no canonical tokens parsed from {PRESETS}", file=sys.stderr)
        return 1

    bad: list[tuple[str, int, str]] = []
    for rel in SCAN_DIRS:
        d = os.path.join(REPO, rel)
        if not os.path.isdir(d):
            continue
        for name in sorted(os.listdir(d)):
            if not name.endswith(".cpp"):
                continue
            path = os.path.join(d, name)
            with open(path, encoding="utf-8") as f:
                for lineno, line in enumerate(f, 1):
                    for key in KEY_RE.findall(line):
                        if key in canon or key in ALLOWLISTED_OVERRIDES:
                            continue
                        bad.append((os.path.join(rel, name), lineno, key))

    if bad:
        print("[token-key] FAIL — resolve_color() with non-canonical token key(s):",
              file=sys.stderr)
        for f, ln, key in bad:
            print(f"  {f}:{ln}  resolve_color(\"{key}\", …)  — not a theme token; "
                  f"it silently renders the hardcoded fallback and won't reskin.",
                  file=sys.stderr)
        print("\nFix: use the real dotted token (see theme_presets.cpp derive_theme), "
              "or, for a deliberate widget-specific override that falls back to a "
              "canonical token, add it to ALLOWLISTED_OVERRIDES in "
              "tools/scripts/token_key_check.py.", file=sys.stderr)
        return 1

    print(f"[token-key] ok — {len(canon)} canonical tokens; all resolve_color keys "
          f"in view paint code resolve to a real token.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
