#!/usr/bin/env python3
"""Tests for the component-contract validator.

The case that motivated it: a meter authored as `.meter-track > .meter-fill`
instead of the real `.level` / `.peak` rendered as an empty box, and the import
still reported 100% similarity, PASS, and every control bound -- because the
browser drew the same empty box we did. Every existing gate was green on a
visibly broken panel, so the negative controls here matter more than the
positive one.
"""
import sys
import pathlib

sys.path.insert(0, str(pathlib.Path(__file__).parent))
from component_contract import parse_contract, check  # noqa: E402

CSS = """
.pulp-knob { display: inline-flex; }
.pulp-knob .dial { width: 64px; }
.pulp-knob .ring { position: absolute; }
.pulp-knob .knob-label { font-size: 10px; }
.pulp-meter { width: 10px; }
.pulp-meter .level { position: absolute; }
.pulp-meter .peak { height: 2px; }
/* .pulp-meter .legacy-fill { } */
"""

failures = []


def expect(name, condition, detail=""):
    if condition:
        print(f"  ok   {name}")
    else:
        print(f"  FAIL {name} {detail}")
        failures.append(name)


components, children = parse_contract(CSS)

expect("components parsed", components == {"pulp-knob", "pulp-meter"}, components)
expect("children parsed", children["pulp-meter"] == {"level", "peak"}, children.get("pulp-meter"))

# A commented-out rule documents history; it must not license the class.
expect("commented rule is not a contract", "legacy-fill" not in children["pulp-meter"])

good = ('<div class="pulp-meter"><div class="level"></div>'
        '<div class="peak"></div></div>')
problems, unknown, _ = check(good, components, children)
expect("real markup passes", not problems and not unknown, problems + unknown)

# THE regression: this exact markup scored 100% similarity / PASS / bound 4-of-4.
bad = ('<div class="pulp-meter"><div class="meter-track">'
       '<div class="meter-fill"></div></div></div>')
problems, unknown, _ = check(bad, components, children)
named = {p[1] for p in problems}
expect("invented children are caught", named == {"meter-track", "meter-fill"}, named)
expect("correction is offered", problems and problems[0][3] == ["level", "peak"],
       problems[0][3] if problems else None)

# An invented COMPONENT hides everything beneath it, so it must fail too.
problems, unknown, _ = check('<div class="pulp-panel-header"><div class="t"></div></div>',
                             components, children)
expect("invented component is caught", [u[0] for u in unknown] == ["pulp-panel-header"], unknown)

# A void element must not unbalance the stack and swallow later checks.
mixed = ('<div class="pulp-knob"><img src="x"><div class="nope"></div></div>')
problems, unknown, _ = check(mixed, components, children)
expect("void elements do not break nesting", {p[1] for p in problems} == {"nope"},
       {p[1] for p in problems})

# Nothing to say about markup that uses no components at all.
problems, unknown, _ = check('<div class="wrapper"><span class="x"></span></div>',
                             components, children)
expect("non-component markup is not policed", not problems and not unknown)

print(f"\n{'FAILED: ' + ', '.join(failures) if failures else 'all checks passed'}")
sys.exit(1 if failures else 0)
