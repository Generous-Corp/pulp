#!/usr/bin/env python3
"""Third gate: a custom property the design system never defines.

The first two gates ask about classes and macros. Neither sees this:

    <div class="pulp-knob" style="--knob-size:112px">

`--knob-size` does not exist. The markup is valid, every class is real, the
macro is bound, the browser renders it, and we match that render exactly --
so pixel-parity, the component contract, and the macro contract are all green.
The only casualty is the design intent: the hero knob is the same size as the
others and nobody is told.

This is the quiet failure mode of authoring against a system you cannot see.
An agent will reach for the property name that *ought* to exist.
"""
from __future__ import annotations

import re

# Custom properties the author SETS, in a style attribute or a <style> block.
SET = re.compile(r"(--[a-zA-Z0-9_-]+)\s*:")
# Custom properties the stylesheet READS, via var(--x).
READ = re.compile(r"var\(\s*(--[a-zA-Z0-9_-]+)")


def system_properties(css: str) -> set[str]:
    """Every custom property the design system defines or consumes.

    A property the stylesheet reads via var() is part of the contract even if
    it is never given a default -- `--_deg` and `--_rot` are exactly that: the
    hooks a knob's value and pointer rotation are meant to be set through.
    """
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    return set(SET.findall(css)) | set(READ.findall(css))


def check_tokens(html: str, known: set[str]):
    """Return [(prop, line)] for properties the author set that nobody reads."""
    bad = []
    for lineno, line in enumerate(html.splitlines(), 1):
        # Only look at authored style, not the whole document, so prose
        # containing "--foo:" cannot be mistaken for a declaration.
        for chunk in re.findall(r'style\s*=\s*"([^"]*)"', line):
            for prop in SET.findall(chunk):
                if prop not in known:
                    bad.append((prop, lineno))
    return bad


if __name__ == "__main__":
    failures = []

    def expect(name, cond, detail=""):
        print(f"  {'ok  ' if cond else 'FAIL'} {name}" + ("" if cond else f" {detail}"))
        if not cond:
            failures.append(name)

    css = """
    :root { --accent: #0aa; --space-2: 4px; }
    .pulp-knob .ring { background: conic-gradient(var(--accent) var(--_deg, 160deg)); }
    .pulp-knob .pointer { transform: rotate(var(--_rot, 0deg)); }
    /* .pulp-knob { --legacy-size: 64px; } */
    """
    known = system_properties(css)

    expect("defined properties are known", "--accent" in known)
    expect("var()-only hooks are known", "--_deg" in known and "--_rot" in known)
    expect("commented properties are not known", "--legacy-size" not in known)

    # THE regression: the invented property that silently lost the hero knob.
    bad = check_tokens('<div class="pulp-knob" style="--knob-size:112px"></div>', known)
    expect("invented property is caught", [p for p, _ in bad] == ["--knob-size"], bad)

    ok = check_tokens('<div class="ring" style="--_deg:210deg"></div>', known)
    expect("real hook passes", not ok, ok)

    # Ordinary CSS must not be policed -- only custom properties.
    ok = check_tokens('<div style="width:112px;height:112px"></div>', known)
    expect("plain declarations are ignored", not ok, ok)

    mixed = check_tokens('<div style="--_deg:200deg;--nope:1px"></div>', known)
    expect("mixed style attr flags only the bad one", [p for p, _ in mixed] == ["--nope"], mixed)

    prose = check_tokens('<p>set --knob-size: to taste</p>', known)
    expect("prose is not a declaration", not prose, prose)

    print(f"\n{'FAILED: ' + ', '.join(failures) if failures else 'all checks passed'}")
    raise SystemExit(1 if failures else 0)
