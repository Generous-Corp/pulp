#!/usr/bin/env python3
"""Realtime-performance contract: no framework commit on a per-frame path.

A scripted editor that shows live audio does three things at display rate at
once -- receives data, reacts to the pointer, paints -- and the costly mistake
is a React commit riding one of those paths. In a materialized/captured import
every commit re-applies the captured document metadata, so a `setHover(...)`
inside `onPointerMove` measured ~42 ms per move against ~0.2 ms for the same
element's `mousemove`. None of this shows in a screenshot, a pixel diff, a
browser capture or a unit test; it shows up as a stall under the mouse.

This gate reads the authored source (HTML, JS, JSX, TSX) and flags, with
file:line:

  hot-setter     a React state setter (from useState / useReducer, or
                 setState) called inside an onPointerMove / onMouseMove /
                 onWheel handler (bubble or capture), a pointermove / mousemove
                 / wheel listener, a requestAnimationFrame callback, a
                 setInterval callback, or a setTimeout callback that re-arms
                 itself -- directly or through local functions it calls.
  fresh-sync     a setter called, unguarded, with a freshly built object or
                 array inside a useEffect / useLayoutEffect that re-runs on
                 updates (`setMacroState(new Array(...))`, `setValue({...v})`).
                 Every run commits, because a fresh object is never equal to
                 the previous state.
  double-phase   the same handler registered for both the capture and bubble
                 phase of one event on one element (runs twice per event).

The remedy for every finding is the view-bridge skill's checklist
(`.agents/skills/view-bridge/SKILL.md`, "Realtime scripted editors: the
performance checklist"): keep pointer/animation state in refs, draw from the
refs, write readouts imperatively, and commit only for structural changes.

What it cannot see (static text analysis, by design):
  * Setters reached through a prop, context, import, or object member
    (`props.onHover(...)`, `store.set(...)`, a custom hook's returned setter
    under a different name). Only names bound by a local useState/useReducer
    destructuring, and bare setState, are recognized.
  * Handlers resolved through anything other than a local
    `function NAME`, `const NAME = (...) =>`, `const NAME = function`, or
    `useCallback(...)` definition; calls are followed through such local
    functions only, a bounded number of levels deep.
  * Whether a guard actually suppresses commits. A setter inside a hot
    handler is flagged even behind `if (next !== prev)`, because on a dense
    target "only when it changes" still commits nearly every move. In an
    effect, any enclosing `if`, `&&`, `?:` counts as a guard.
  * Updater functions that always return a fresh value
    (`setX(prev => [...prev])`), and fresh values built in a variable first
    (`const next = {...}; setX(next)`).
  * Inline HTML event attributes (`onpointermove="..."`) and listeners added
    through a bare `addEventListener(...)` with no receiver.
  * Native-side costs: `load_script` per tick, un-throttled readouts, a
    VisualizationBridge backlog policy. Those live in C++ and in the host.
A clean run means none of the recognized shapes appear -- never that the
editor holds its frame rate. Prove that with a trace (trace-analysis skill).
"""
from __future__ import annotations

import re
from dataclasses import dataclass

CHECKLIST = ('.agents/skills/view-bridge/SKILL.md '
             '"Realtime scripted editors: the performance checklist"')

# JSX props whose handler runs per pointer sample or wheel tick.
HOT_PROPS = ("onPointerMove", "onPointerMoveCapture", "onMouseMove",
             "onMouseMoveCapture", "onWheel", "onWheelCapture",
             "onPointerRawUpdate")
HOT_DOM_EVENTS = ("pointermove", "mousemove", "wheel", "pointerrawupdate")
MAX_CALL_DEPTH = 3


@dataclass(frozen=True)
class Finding:
    rule: str
    line: int
    message: str


# ── lexing helpers ─────────────────────────────────────────────────────────

def strip_comments(src: str) -> str:
    """Blank out JS and HTML comments, preserving offsets and newlines.

    Strings and template literals are kept verbatim so later scans can still
    skip over them; a regex literal containing a quote is not modelled.
    """
    out = list(src)
    i, n = 0, len(src)

    def blank(a: int, b: int) -> None:
        for k in range(a, min(b, n)):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = src[i]
        if src.startswith("<!--", i):
            end = src.find("-->", i + 4)
            end = n if end < 0 else end + 3
            blank(i, end)
            i = end
        elif src.startswith("//", i):
            end = src.find("\n", i)
            end = n if end < 0 else end
            blank(i, end)
            i = end
        elif src.startswith("/*", i):
            end = src.find("*/", i + 2)
            end = n if end < 0 else end + 2
            blank(i, end)
            i = end
        elif c in "'\"`":
            i = _skip_string(src, i)
        else:
            i += 1
    return "".join(out)


def _skip_string(src: str, i: int) -> int:
    quote, j, n = src[i], i + 1, len(src)
    while j < n:
        if src[j] == "\\":
            j += 2
            continue
        if src[j] == quote:
            return j + 1
        if quote != "`" and src[j] == "\n":
            return j  # unterminated ordinary string: stop at the line end
        j += 1
    return n


def find_close(src: str, open_idx: int) -> int:
    """Index of the bracket matching src[open_idx], or len(src)."""
    pairs = {"(": ")", "{": "}", "[": "]"}
    stack = [pairs[src[open_idx]]]
    i, n = open_idx + 1, len(src)
    while i < n:
        c = src[i]
        if c in "'\"`":
            i = _skip_string(src, i)
            continue
        if c in pairs:
            stack.append(pairs[c])
        elif c in ")}]":
            if not stack or c != stack[-1]:
                return n
            stack.pop()
            if not stack:
                return i
        i += 1
    return n


def line_of(src: str, idx: int) -> int:
    return src.count("\n", 0, idx) + 1


# ── recognizers ────────────────────────────────────────────────────────────

STATE_HOOK = re.compile(
    r"\[\s*[A-Za-z_$][\w$]*\s*,\s*([A-Za-z_$][\w$]*)\s*\]\s*=\s*"
    r"(?:React\s*\.\s*)?use(?:State|Reducer)\b")


def state_setters(src: str) -> set[str]:
    """Names bound as the second element of a useState/useReducer pair."""
    return set(STATE_HOOK.findall(src))


def _setter_call(setters: set[str]) -> re.Pattern | None:
    names = sorted(setters | {"setState"}, key=len, reverse=True)
    return re.compile(r"(?<![\w$.])(?:this\s*\.\s*)?(" +
                      "|".join(re.escape(s) for s in names) + r")\s*\(")


def _function_bodies(src: str) -> dict[str, tuple[int, int]]:
    """Map local function names to the (start, end) span of their body."""
    spans: dict[str, tuple[int, int]] = {}
    for m in re.finditer(r"\bfunction\s*\*?\s*([A-Za-z_$][\w$]*)\s*\(", src):
        params_end = find_close(src, m.end() - 1)
        brace = src.find("{", params_end)
        if brace >= 0:
            spans.setdefault(m.group(1), (brace, find_close(src, brace)))
    assign = re.compile(
        r"(?:\b(?:const|let|var)\s+)?([A-Za-z_$][\w$]*)\s*=\s*"
        r"(?:(?:React\s*\.\s*)?use(?:Callback|Event)\s*\(\s*)?"
        r"(?:async\s+)?(function\b[^(]*\(|\(|[A-Za-z_$][\w$]*\s*=>)")
    for m in assign.finditer(src):
        name, head = m.group(1), m.group(2)
        if name in spans:
            continue
        if head.startswith("function") or head == "(":
            params_end = find_close(src, m.end() - 1)
            rest = src[params_end + 1:]
            arrow = re.match(r"\s*(?::[^=]*?)?=>\s*", rest)
            if head == "(" and not arrow:
                continue  # `x = (a + b)` is not a function
            body_at = params_end + 1 + (arrow.end() if arrow else 0)
        else:
            body_at = m.end()
        spans[name] = _body_span(src, body_at)
    return spans


def _body_span(src: str, at: int) -> tuple[int, int]:
    while at < len(src) and src[at] in " \t\r\n":
        at += 1
    if at < len(src) and src[at] == "{":
        return at, find_close(src, at)
    # Expression body: runs to the first unbalanced closer or statement end.
    i, depth = at, 0
    while i < len(src):
        c = src[i]
        if c in "'\"`":
            i = _skip_string(src, i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            if depth == 0:
                break
            depth -= 1
        elif c in ";,\n" and depth == 0:
            break
        i += 1
    return at, i


@dataclass
class _Scan:
    src: str
    setter_re: re.Pattern
    functions: dict[str, tuple[int, int]]

    def setters_in(self, start: int, end: int, depth: int = 0,
                   via: tuple[str, ...] = (), seen: frozenset = frozenset()):
        """Yield (setter, index, via-chain) for commits reachable from a span."""
        body = self.src[start:end]
        for m in self.setter_re.finditer(body):
            yield m.group(1), start + m.start(1), via
        if depth >= MAX_CALL_DEPTH:
            return
        for m in re.finditer(r"(?<![\w$.])([A-Za-z_$][\w$]*)\s*\(", body):
            name = m.group(1)
            if name in seen or name not in self.functions:
                continue
            fs, fe = self.functions[name]
            if fs <= start + m.start() <= fe:
                continue  # recursion into the enclosing function itself
            yield from self.setters_in(fs, fe, depth + 1, via + (name,),
                                       seen | {name})

    def handler_span(self, expr_start: int, expr_end: int):
        """Resolve a handler expression to the span that runs per event."""
        expr = self.src[expr_start:expr_end].strip()
        ident = re.fullmatch(r"(?:this\s*\.\s*)?([A-Za-z_$][\w$]*)", expr)
        if ident:
            name = ident.group(1)
            if name in self.functions:
                return (*self.functions[name], name)
            return None
        return expr_start, expr_end, None


def _hot_regions(scan: _Scan):
    """Yield (kind, span_start, span_end, handler_name) for per-event code."""
    src = scan.src
    prop = re.compile(r"\b(" + "|".join(HOT_PROPS) + r")\s*=\s*\{")
    for m in prop.finditer(src):
        close = find_close(src, m.end() - 1)
        resolved = scan.handler_span(m.end(), close)
        if resolved:
            yield f"an {m.group(1)} handler", *resolved
    listener = re.compile(
        r"\.\s*addEventListener\s*\(\s*(['\"])(" + "|".join(HOT_DOM_EVENTS) +
        r")\1\s*,")
    for m in listener.finditer(src):
        close = find_close(src, src.rfind("(", m.start(), m.end()))
        arg = _first_argument(src, m.end(), close)
        resolved = scan.handler_span(*arg)
        if resolved:
            yield f"a '{m.group(2)}' listener", *resolved
    for api, kind in (("requestAnimationFrame", "a requestAnimationFrame callback"),
                      ("setInterval", "a setInterval callback")):
        for m in re.finditer(r"(?<![\w$])" + api + r"\s*\(", src):
            close = find_close(src, m.end() - 1)
            arg = _first_argument(src, m.end(), close)
            resolved = scan.handler_span(*arg)
            if resolved:
                yield kind, *resolved
    # A setTimeout that re-arms itself fires repeatedly; a one-shot does not.
    for name, (fs, fe) in scan.functions.items():
        body = src[fs:fe]
        if re.search(r"(?<![\w$])setTimeout\s*\(\s*" + re.escape(name) + r"\b",
                     body):
            yield "a self-re-arming setTimeout callback", fs, fe, name


def _first_argument(src: str, start: int, close: int) -> tuple[int, int]:
    i, depth = start, 0
    while i < close:
        c = src[i]
        if c in "'\"`":
            i = _skip_string(src, i)
            continue
        if c in "([{":
            depth += 1
        elif c in ")]}":
            depth -= 1
        elif c == "," and depth == 0:
            break
        i += 1
    return start, i


FRESH_ARG = re.compile(
    r"\s*(?:\[|\{|new\s+[A-Z]\w*|Array\s*\.\s*(?:from|of)\s*\(|Object\s*\.\s*"
    r"(?:assign|fromEntries)\s*\(|[\w$.\]\)]+\s*\.\s*(?:map|filter|slice|concat)\s*\()")


def _effect_spans(src: str):
    """Yield (body_start, body_end) for effects that re-run on updates."""
    for m in re.finditer(r"(?<![\w$])(?:React\s*\.\s*)?use(?:Layout)?Effect\s*\(",
                         src):
        close = find_close(src, m.end() - 1)
        fn_start, fn_end = _first_argument(src, m.end(), close)
        deps = src[fn_end:close].strip().lstrip(",").strip()
        if re.fullmatch(r"\[\s*\]", deps):
            continue  # mount-only effect: runs once
        arrow = src.find("=>", fn_start, fn_end)
        if arrow < 0:
            continue
        yield _body_span(src, arrow + 2)


def _guarded(src: str, body_start: int, call: int) -> bool:
    """True when the call sits under an if/&&/?:/|| inside the effect body."""
    for m in re.finditer(r"(?<![\w$])if\s*\(", src[body_start:call]):
        cond_end = find_close(src, body_start + m.end() - 1)
        after = cond_end + 1
        while after < len(src) and src[after] in " \t\r\n":
            after += 1
        if after < len(src) and src[after] == "{":
            if after <= call <= find_close(src, after):
                return True
        else:
            stmt_end = src.find(";", after)
            newline = src.find("\n", after)
            ends = [e for e in (stmt_end, newline) if e >= 0]
            if after <= call <= (min(ends) if ends else len(src)):
                return True
    stmt_start = max(src.rfind(";", body_start, call),
                     src.rfind("\n", body_start, call),
                     src.rfind("{", body_start, call))
    return bool(re.search(r"&&|\|\||\?", src[stmt_start + 1:call]))


def _double_phase(src: str):
    """Yield (index, event, handler) for one handler bound to both phases."""
    for m in re.finditer(r"\bon([A-Z]\w*?)Capture\s*=\s*\{", src):
        event = m.group(1)
        close = find_close(src, m.end() - 1)
        handler = src[m.end():close].strip()
        if not handler:
            continue
        tag_start = src.rfind("<", 0, m.start())
        tag_end = _tag_end(src, tag_start) if tag_start >= 0 else len(src)
        tag = src[tag_start:tag_end]
        bubble = re.search(r"\bon" + re.escape(event) + r"\s*=\s*\{", tag)
        if bubble:
            b_open = tag_start + bubble.end() - 1
            if src[b_open + 1:find_close(src, b_open)].strip() == handler:
                yield m.start(), event, handler
    listener = re.compile(
        r"([\w$.]+)\s*\.\s*addEventListener\s*\(\s*(['\"])(\w+)\2\s*,\s*"
        r"([A-Za-z_$][\w$.]*)\s*(,\s*([^)]*))?\)")
    seen: dict[tuple[str, str, str], tuple[int, bool]] = {}
    for m in listener.finditer(src):
        target, event, handler, opts = m.group(1), m.group(3), m.group(4), m.group(6)
        capture = bool(opts) and bool(re.search(r"\btrue\b|capture\s*:\s*true",
                                                opts))
        key = (target, event, handler)
        if key in seen and seen[key][1] != capture:
            yield m.start(), event, handler
        seen.setdefault(key, (m.start(), capture))


def _tag_end(src: str, tag_start: int) -> int:
    i, n = tag_start + 1, len(src)
    while i < n:
        c = src[i]
        if c in "'\"`":
            i = _skip_string(src, i)
            continue
        if c == "{":
            i = find_close(src, i) + 1
            continue
        if c == ">":
            return i
        i += 1
    return n


# ── entry point ────────────────────────────────────────────────────────────

def check_realtime(source: str) -> list[Finding]:
    """Return realtime-contract findings for one authored source file."""
    src = strip_comments(source)
    setters = state_setters(src)
    setter_re = _setter_call(setters)
    scan = _Scan(src, setter_re, _function_bodies(src))
    findings: dict[tuple[str, int], Finding] = {}

    for kind, start, end, name in _hot_regions(scan):
        seen = frozenset({name}) if name else frozenset()
        for setter, at, via in scan.setters_in(start, end, seen=seen):
            chain = ((name,) if name else ()) + via
            through = f" (via {' -> '.join(chain)})" if chain else ""
            key = ("hot-setter", at)
            findings.setdefault(key, Finding(
                "hot-setter", line_of(src, at),
                f"{setter}() commits React state inside {kind}{through} -- "
                f"keep it in a ref and write the canvas/DOM directly; see "
                f"{CHECKLIST}"))

    for body_start, body_end in _effect_spans(src):
        for m in setter_re.finditer(src, body_start, body_end):
            arg_at = m.end()
            if not FRESH_ARG.match(src, arg_at):
                continue
            if _guarded(src, body_start, m.start()):
                continue
            findings.setdefault(("fresh-sync", m.start()), Finding(
                "fresh-sync", line_of(src, m.start()),
                f"{m.group(1)}() is called with a fresh object/array on every "
                f"effect run, so every update commits -- compare against the "
                f"current value first and skip the call when equal; see "
                f"{CHECKLIST}"))

    for at, event, handler in _double_phase(src):
        findings.setdefault(("double-phase", at), Finding(
            "double-phase", line_of(src, at),
            f"{handler} is registered for both the capture and bubble phase of "
            f"{event} and runs twice per event -- register it once; see "
            f"{CHECKLIST}"))

    return sorted(findings.values(), key=lambda f: (f.line, f.rule))
