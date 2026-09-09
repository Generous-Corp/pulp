#!/usr/bin/env python3
"""Derive which CSS properties may cache their last applied value.

`CSSStyleDeclaration._applyProperty` skips a write whose raw string equals the
one it last applied for that property. That is only sound when no *other*
property can have overwritten the same widget state in the meantime, so the
guard consults a table saying, for each property, which other properties share
its state.

The table is derived from the handler sources rather than maintained by hand: a
hand-written alias list fails silently and visually when it falls behind the
code, while a derived one fails loudly in CI the moment a handler starts
writing a slot it did not write before.

Derivation, per `case "<prop>":` block of the `_apply*Prop` switches:

  * collect every call, expanding calls to prelude-defined functions
    transitively so a helper's bridge calls are attributed to the property;
  * keep only names the C++ side actually registers as bridge globals
    (`register_bridge_function`), so ordinary JS locals are not mistaken for
    widget state;
  * name the state each call writes -- its "slot" -- as the function name plus,
    for sub-keyed setters like `setFlex(id, "margin_top", v)`, the literal
    sub-key. A computed sub-key is recorded as `*`, which conflicts with every
    sub-key of that function.

Properties whose slots are disjoint from every other property's are singletons
and dedup freely. Properties that share a slot form a group: applying one drops
the cached values of the rest. Properties with a `*` slot (shorthands that
expand over a computed edge) and properties this script could not classify are
absent from the table, which the runtime treats as "never dedup, and drop the
whole cache on apply" -- the safe direction, costing only missed optimisation.
"""

import argparse
import json
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
JS_DIR = os.path.join(REPO, "core", "view", "js")
CPP_DIRS = [os.path.join(REPO, "core", "view", "src")]
OUT = os.path.join(JS_DIR, "web-compat-style-dedup-table.js")
HANDLER_GLOB = "web-compat-style-decl"


def mask_strings(s):
    """Blank parentheses inside string literals so they are not read as calls."""
    out, i, n = [], 0, len(s)
    while i < n:
        c = s[i]
        if c in "\"'":
            q = c
            out.append(c)
            i += 1
            while i < n and s[i] != q:
                ch = s[i]
                if ch == "\\":
                    out.append(ch)
                    i += 1
                    ch = s[i] if i < n else ""
                out.append("\x01" if ch in "()" else ch)
                i += 1
            if i < n:
                out.append(q)
                i += 1
        else:
            out.append(c)
            i += 1
    return "".join(out)


def strip_comments(s):
    s = re.sub(r"/\*.*?\*/", "", s, flags=re.S)
    s = re.sub(r"(^|[^:])//[^\n]*", lambda m: m.group(1), s)
    return mask_strings(s)


def bridge_names():
    names = set()
    pat = re.compile(r'register_bridge_function\s*\(\s*[A-Za-z_]\w*\s*,\s*"([A-Za-z0-9_]+)"')
    for root in CPP_DIRS:
        for dirpath, _dirs, files in os.walk(root):
            for f in files:
                if not f.endswith((".cpp", ".hpp", ".h")):
                    continue
                with open(os.path.join(dirpath, f), encoding="utf-8", errors="replace") as fh:
                    names |= set(pat.findall(fh.read()))
    return names


def brace_body(text, open_idx):
    """Text between the brace at `open_idx` and its match."""
    d, i = 0, open_idx
    while i < len(text):
        if text[i] == "{":
            d += 1
        elif text[i] == "}":
            d -= 1
            if d == 0:
                return text[open_idx + 1:i]
        i += 1
    return text[open_idx + 1:]


def function_bodies(sources):
    """Name -> body for every function a handler case could call into.

    Both declarations (`function f() {}`) and prototype methods
    (`X.prototype.f = function () {}`) count: a case that calls
    `this._reevaluateOverlay()` reaches `claimOverlay` on the bridge just as
    surely as one that calls a free function, and missing that would leave two
    properties that share a slot in separate groups.
    """
    bodies = {}
    patterns = (
        r"\bfunction\s+([A-Za-z_$][\w$]*)\s*\([^)]*\)\s*\{",
        r"\.prototype\.([A-Za-z_$][\w$]*)\s*=\s*function\s*\([^)]*\)\s*\{",
    )
    for text in sources.values():
        for pat in patterns:
            for m in re.finditer(pat, text):
                bodies[m.group(1)] = brace_body(text, m.end() - 1)
    return bodies


def calls(text):
    """(name, literal-second-arg or '*' or None, dotted) for each call in `text`.

    `dotted` marks a member call (`decl._reevaluateOverlay()`, `s.trim()`). A
    dotted name is never a bridge global, but it can still be a repo-defined
    prototype method whose body reaches the bridge, so the caller resolves it
    against the known function bodies only.
    """
    res = []
    for m in re.finditer(r"([A-Za-z_$][\w$]*)\s*\(", text):
        name = m.group(1)
        prev = text[m.start(1) - 1] if m.start(1) else ""
        # A leading identifier character means this is the tail of a longer
        # name, not a call. A leading `.` means a member call -- and the dot
        # must be tested separately from the "not an identifier char" test,
        # because a single regex alternation lets `RE.exec(` match with the
        # dot consumed as the boundary, which reads a builtin as undotted.
        if prev and (prev.isalnum() or prev in "_$"):
            continue
        dotted = prev == "."
        i, d, start = m.end(), 1, m.end()
        while i < len(text) and d > 0:
            if text[i] == "(":
                d += 1
            elif text[i] == ")":
                d -= 1
            i += 1
        args, parts, depth, cur = text[start:i - 1], [], 0, ""
        for ch in args:
            if ch in "([{":
                depth += 1
            elif ch in ")]}":
                depth -= 1
            if ch == "," and depth == 0:
                parts.append(cur)
                cur = ""
            else:
                cur += ch
        parts.append(cur)
        parts = [p.strip() for p in parts]
        sub = None
        if len(parts) >= 3:
            lit = re.fullmatch(r"\"([^\"]*)\"|'([^']*)'", parts[1])
            sub = (lit.group(1) or lit.group(2)) if lit else "*"
        res.append((name, sub, dotted))
    return res


def slots_for(text, bodies, bridge, depth=0, seen=frozenset()):
    found = set()
    for name, sub, dotted in calls(text):
        if name in bridge and not dotted:
            # A `get*` bridge call reads widget/theme state; only a write can
            # invalidate another property's cached value. Counting reads as
            # slots would merge every property that resolves a `var()` into one
            # group and erase most of the optimisation.
            if not name.startswith("get"):
                found.add(name + ("|" + sub if sub else ""))
        # A member call resolves only for a repo-internal `_name` method.
        # Anything else dotted is a builtin (`re.exec`, `s.trim`) whose name
        # can collide with a repo function and drag in slots the case never
        # writes -- which over-merges groups and quietly costs the optimisation.
        elif (not dotted or name.startswith("_")) \
                and name in bodies and name not in seen and depth < 8:
            found |= slots_for(bodies[name], bodies, bridge, depth + 1, seen | {name})
    return found


def property_slots():
    sources = {}
    for f in sorted(os.listdir(JS_DIR)):
        if f.endswith(".js"):
            with open(os.path.join(JS_DIR, f), encoding="utf-8") as fh:
                sources[f] = strip_comments(fh.read())
    bodies = function_bodies(sources)
    bridge = bridge_names()
    if not bridge:
        raise SystemExit("no bridge functions found -- the extractor is broken, refusing to emit")

    props = {}
    for f, text in sources.items():
        if HANDLER_GLOB not in f:
            continue
        fn = re.search(r"function\s+_apply\w+Prop\s*\([^)]*\)\s*\{", text)
        if not fn:
            continue
        sw = re.compile(r"switch\s*\(\s*key\s*\)\s*\{").search(text, fn.end())
        if not sw:
            continue
        body = brace_body(text, sw.end() - 1)
        labels = [(m.start(), m.group(1)) for m in re.finditer(r'case\s+"([^"]+)"\s*:', body)]
        for idx, (pos, name) in enumerate(labels):
            end = labels[idx + 1][0] if idx + 1 < len(labels) else len(body)
            seg = body[body.index(":", pos) + 1:end]
            props.setdefault(name, set())
            props[name] |= slots_for(seg, bodies, bridge)
        # `case "a": case "b": <body>` — an empty label shares the next body.
        for idx, (_pos, name) in enumerate(labels):
            if props[name]:
                continue
            for j in range(idx + 1, len(labels)):
                if props[labels[j][1]]:
                    props[name] = set(props[labels[j][1]])
                    break
    return props


def partition(props):
    wild = {p for p, s in props.items() if any(x.endswith("|*") for x in s)}
    cand = {p: s for p, s in props.items() if p not in wild}
    names = sorted(cand)
    parent = {p: p for p in names}

    def find(x):
        while parent[x] != x:
            parent[x] = parent[parent[x]]
            x = parent[x]
        return x

    for i, a in enumerate(names):
        for b in names[i + 1:]:
            if cand[a] & cand[b]:
                ra, rb = find(a), find(b)
                if ra != rb:
                    parent[ra] = rb
    groups = {}
    for p in names:
        groups.setdefault(find(p), []).append(p)
    return sorted((sorted(v) for v in groups.values()), key=lambda g: (-len(g), g[0]))


def render(groups):
    lines = [
        "// GENERATED by tools/scripts/style_dedup_table.py -- do not edit by hand.",
        "//",
        "// Which CSS properties may cache their last applied value, and which",
        "// properties share widget state with them. Derived from the bridge calls in",
        "// the `_apply*Prop` handlers; regenerate with `--write` after changing what a",
        "// handler writes. A property missing from `_DEDUP_GROUP` is never deduped and",
        "// drops the element's whole cache when it applies, so an omission costs speed",
        "// rather than correctness.",
        "",
        "// Preludes evaluate separately, so a top-level `var` stays local to this",
        "// file; the table is published on globalThis for _applyProperty to read.",
        "globalThis._DEDUP_MEMBERS = [",
    ]
    for g in groups:
        lines.append("    " + json.dumps(g) + ",")
    lines.append("];")
    lines.append("")
    lines.append("globalThis._DEDUP_GROUP = {")
    for i, g in enumerate(groups):
        for p in g:
            lines.append("    %s: %d," % (json.dumps(p), i))
    lines.append("};")
    lines.append("")
    return "\n".join(lines)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--write", action="store_true", help="regenerate the table")
    ap.add_argument("--dump", action="store_true", help="print the derived slots")
    args = ap.parse_args()

    props = property_slots()
    if args.dump:
        json.dump({k: sorted(v) for k, v in sorted(props.items())}, sys.stdout, indent=1)
        print()
        return 0
    groups = partition(props)
    text = render(groups)
    if args.write:
        with open(OUT, "w", encoding="utf-8") as fh:
            fh.write(text)
        singles = sum(1 for g in groups if len(g) == 1)
        print("wrote %s: %d groups (%d singleton), %d properties"
              % (os.path.relpath(OUT, REPO), len(groups), singles, sum(len(g) for g in groups)))
        return 0
    if not os.path.exists(OUT):
        print("missing %s -- run with --write" % OUT, file=sys.stderr)
        return 1
    with open(OUT, encoding="utf-8") as fh:
        have = fh.read()
    if have != text:
        print("%s is stale: a style handler changed which bridge slots it writes.\n"
              "Regenerate with: python3 tools/scripts/style_dedup_table.py --write"
              % os.path.relpath(OUT, REPO), file=sys.stderr)
        return 1
    print("style dedup table up to date")
    return 0


if __name__ == "__main__":
    sys.exit(main())
