#!/usr/bin/env python3
"""Check authored HTML against a design system's component contract.

Pixel-parity cannot catch an invented class name. A knob authored with a child
the stylesheet never defines renders as an unstyled box -- and the browser
renders that same box, so our render matches it exactly and the import reports
100% similarity and PASS. The panel is wrong and every gate is green.

This validator closes that hole by asking a different question: does each
component's markup use only children the stylesheet actually styles?

    python3 component_contract.py --system <dir-with-components.css> panel.html

Exit 0 when every component checks out, 1 when any element uses a child class
its component does not define.
"""
from __future__ import annotations

import argparse
import pathlib
import re
import sys
from html.parser import HTMLParser

# `.pulp-knob .dial`, `.pulp-meter .level`, `.pulp-knob .ring` ...
DESCENDANT = re.compile(r"\.(pulp-[a-z0-9-]+)\s+\.([a-z0-9-]+)")
# `.pulp-knob { ... }` -- the component itself
COMPONENT = re.compile(r"\.(pulp-[a-z0-9-]+)\s*[,{]")


def parse_contract(css: str) -> tuple[set[str], dict[str, set[str]]]:
    """Return (known components, component -> classes it styles as descendants).

    Comments are stripped first: a commented-out rule documents history, not a
    class the stylesheet will actually paint, and admitting it would let an
    invented name pass because someone once wrote it down.
    """
    css = re.sub(r"/\*.*?\*/", "", css, flags=re.S)
    components = set(COMPONENT.findall(css))
    children: dict[str, set[str]] = {}
    for component, child in DESCENDANT.findall(css):
        children.setdefault(component, set()).add(child)
    return components, children


class _Panel(HTMLParser):
    """Collect (component, child-classes) pairs from the authored markup."""

    def __init__(self, components: set[str]) -> None:
        super().__init__(convert_charrefs=True)
        self._components = components
        self._stack: list[str | None] = []
        # component -> classes seen beneath it, with the line for the message
        self.seen: list[tuple[str, str, int]] = []
        self.unknown: list[tuple[str, int]] = []

    def handle_starttag(self, tag, attrs):
        classes = ""
        for name, value in attrs:
            if name == "class":
                classes = value or ""
        names = classes.split()
        component = next((c for c in names if c in self._components), None)

        # A class under an open component is part of that component's markup.
        owner = next((c for c in reversed(self._stack) if c), None)
        line = self.getpos()[0]
        # An invented COMPONENT name is the same failure one level up, and it
        # hides the invented children beneath it: nothing styles them either.
        for n in names:
            if n.startswith("pulp-") and n not in self._components:
                self.unknown.append((n, line))
        if owner is not None:
            for n in names:
                if not n.startswith("pulp-"):
                    self.seen.append((owner, n, line))

        self._stack.append(component)
        # Void elements never close, so they must not stay on the stack.
        if tag in {"img", "br", "hr", "input", "meta", "link", "source"}:
            self._stack.pop()

    def handle_endtag(self, tag):
        if self._stack:
            self._stack.pop()


def check(html: str, components: set[str], children: dict[str, set[str]]):
    panel = _Panel(components)
    panel.feed(html)
    problems = []
    unknown = panel.unknown
    for component, child, line in panel.seen:
        allowed = children.get(component, set())
        if child not in allowed:
            problems.append((component, child, line, sorted(allowed)))
    return problems, unknown, len(panel.seen)


def main() -> int:
    ap = argparse.ArgumentParser()
    ap.add_argument("html", type=pathlib.Path)
    ap.add_argument("--system", type=pathlib.Path, required=True,
                    help="directory containing components.css")
    args = ap.parse_args()

    css_path = args.system / "components.css"
    if not css_path.exists():
        print(f"no components.css under {args.system}", file=sys.stderr)
        return 2

    components, children = parse_contract(css_path.read_text())
    problems, unknown, checked = check(args.html.read_text(), components, children)

    for name, line in unknown:
        print(f"{args.html}:{line}: '.{name}' is not a component in this "
              f"design system — nothing styles it or anything inside it")

    if not problems and not unknown:
        print(f"component contract OK — {checked} class(es) checked against "
              f"{len(components)} components")
        return 0

    for component, child, line, allowed in problems:
        near = ", ".join(f".{a}" for a in allowed[:6]) or "(none)"
        print(f"{args.html}:{line}: .{component} has no child class "
              f"'.{child}' — it styles: {near}")
    print(f"\n{len(problems) + len(unknown)} invented class(es). These render as unstyled "
          f"boxes that pixel-parity cannot see: the browser draws the same "
          f"empty box, so the import still reports 100% similarity and PASS.")
    return 1


if __name__ == "__main__":
    sys.exit(main())
