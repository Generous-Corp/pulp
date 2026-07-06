#!/usr/bin/env python3
"""Fail if a Catch2 assertion macro is invoked inside a worker-thread lambda.

Catch2 (pinned at 3.7.1 in this repo) assertion macros are NOT thread-safe:
REQUIRE / CHECK / FAIL and friends mutate the run's assertion counters and the
active-section state with no synchronization. Invoking them from a thread other
than the one that entered the TEST_CASE is undefined behavior — it happens to
pass on bare-metal runners but corrupts under different scheduler timing. That
UB stayed invisible until the required macOS CI gate moved onto sandboxed Tart
VMs, whose thread interleaving reliably tripped it (a HotSwapSlot hammer test
"failed" in 0.00s though the code under test was race-free by design).

Catch2 only gained opt-in thread-safe assertions in 3.9.0. Until this repo pins
that (and enables it), the required pattern for a concurrency test is:

    std::atomic<bool> bad{false};
    std::thread worker([&]{
        if (!invariant_holds()) bad.store(true, std::memory_order_relaxed);
    });
    worker.join();
    REQUIRE_FALSE(bad.load());   // assert on the test thread, after join()

This linter flags assertion macros lexically inside a `std::thread`,
`std::jthread`, or `std::async` lambda body in test/ sources. It is a
best-effort lexical scan: it does not follow calls into helper functions, and a
verified-safe line can be suppressed with a trailing `// thread-assert:allow`
comment. (INFO/CAPTURE message scopes are also thread-unsafe but are not flagged
here — the assertion macros are the demonstrated corruption vector.)
"""

from __future__ import annotations

import re
import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
TEST_DIR = REPO_ROOT / "test"

# Assertion macros that mutate Catch2's per-run state.
ASSERT_MACROS = (
    "REQUIRE_FALSE", "REQUIRE_THROWS_AS", "REQUIRE_THROWS_WITH",
    "REQUIRE_THROWS_MATCHES", "REQUIRE_THROWS", "REQUIRE_NOTHROW",
    "REQUIRE_THAT", "REQUIRE",
    "CHECK_FALSE", "CHECK_THROWS_AS", "CHECK_THROWS_WITH",
    "CHECK_THROWS_MATCHES", "CHECK_THROWS", "CHECK_NOTHROW",
    "CHECK_THAT", "CHECK",
    "FAIL_CHECK", "FAIL", "SUCCEED",
)
# Longest-first alternation so REQUIRE_FALSE wins over REQUIRE, etc. Require a
# following '(' so identifiers like a variable named REQUIRED never match.
ASSERT_RE = re.compile(
    r"\b(" + "|".join(ASSERT_MACROS) + r")\s*\(",
)
THREAD_RE = re.compile(r"\bstd::(?:thread|jthread|async)\b")
SUPPRESS = "thread-assert:allow"


def blank_noncode(text: str) -> str:
    """Return same-length text with string/char/comment bodies blanked to
    spaces (newlines preserved) so brace matching and macro scanning see only
    real code."""
    out = list(text)
    i, n = 0, len(text)
    state = None  # None | 'line' | 'block' | 'str' | 'char'
    while i < n:
        c = text[i]
        nxt = text[i + 1] if i + 1 < n else ""
        if state is None:
            if c == "/" and nxt == "/":
                state = "line"; out[i] = out[i + 1] = " "; i += 2; continue
            if c == "/" and nxt == "*":
                state = "block"; out[i] = out[i + 1] = " "; i += 2; continue
            if c == '"':
                state = "str"; out[i] = " "; i += 1; continue
            if c == "'":
                # A C++ digit separator (10'000, 0x1'FFFF) sits between two
                # alphanumerics — it is NOT a char-literal delimiter. Treating
                # it as one would blank the rest of the line and desync brace
                # matching.
                prev = text[i - 1] if i > 0 else ""
                if prev.isalnum() and nxt.isalnum():
                    i += 1; continue
                state = "char"; out[i] = " "; i += 1; continue
            i += 1; continue
        # inside a blanked region
        if state == "line":
            if c == "\n":
                state = None
            else:
                out[i] = " "
            i += 1; continue
        if state == "block":
            if c == "*" and nxt == "/":
                out[i] = out[i + 1] = " "; i += 2; state = None; continue
            if c != "\n":
                out[i] = " "
            i += 1; continue
        # str / char body
        if c == "\\":
            # Blank the backslash and its escaped char (but never a newline).
            out[i] = " "
            if i + 1 < n and text[i + 1] != "\n":
                out[i + 1] = " "; i += 2
            else:
                i += 1
            continue
        if (state == "str" and c == '"') or (state == "char" and c == "'"):
            out[i] = " "; state = None; i += 1; continue
        if c != "\n":
            out[i] = " "
        i += 1
    return "".join(out)


def lambda_body_span(code: str, start: int) -> tuple[int, int] | None:
    """Given a std::thread/async match end offset, find the enclosing lambda's
    body brace span. Returns (open_brace_idx, close_brace_idx) or None when
    there is no inline lambda (e.g. std::thread t(named_fn, arg))."""
    n = len(code)
    i = start
    # Find the capture-list '[' before any '{' or ';'. If '{' or ';' comes
    # first, this thread has no inline lambda.
    while i < n:
        c = code[i]
        if c == "[":
            break
        if c in "{;":
            return None
        i += 1
    else:
        return None
    # From the capture list, the lambda body is the first '{' at brace depth 0
    # (params/return-type carry no braces).
    depth_sq = 0
    j = i
    while j < n:
        c = code[j]
        if c == "[":
            depth_sq += 1
        elif c == "]":
            depth_sq -= 1
        elif c == "{" and depth_sq == 0:
            break
        elif c == ";" and depth_sq == 0:
            return None
        j += 1
    else:
        return None
    open_idx = j
    depth = 0
    while j < n:
        c = code[j]
        if c == "{":
            depth += 1
        elif c == "}":
            depth -= 1
            if depth == 0:
                return (open_idx, j)
        j += 1
    return None


def line_of(text: str, off: int) -> int:
    return text.count("\n", 0, off) + 1


def scan_file(path: Path) -> list[tuple[int, str]]:
    raw = path.read_text(encoding="utf-8", errors="replace")
    code = blank_noncode(raw)
    raw_lines = raw.splitlines()
    hits: list[tuple[int, str]] = []
    seen_spans: list[tuple[int, int]] = []
    for m in THREAD_RE.finditer(code):
        span = lambda_body_span(code, m.end())
        if span is None:
            continue
        open_idx, close_idx = span
        # Avoid double-reporting nested std::thread bodies: skip if fully
        # contained in an already-scanned span.
        if any(o <= open_idx and close_idx <= c for (o, c) in seen_spans):
            continue
        seen_spans.append(span)
        for am in ASSERT_RE.finditer(code, open_idx, close_idx):
            ln = line_of(raw, am.start())
            src = raw_lines[ln - 1] if ln - 1 < len(raw_lines) else ""
            if SUPPRESS in src:
                continue
            hits.append((ln, src.strip()))
    return hits


def main() -> int:
    if not TEST_DIR.is_dir():
        print(f"thread_assert_check: no test dir at {TEST_DIR}", file=sys.stderr)
        return 0
    files = sorted(TEST_DIR.rglob("*.cpp")) + sorted(TEST_DIR.rglob("*.mm"))
    total = 0
    for path in files:
        hits = scan_file(path)
        if not hits:
            continue
        total += len(hits)
        rel = path.relative_to(REPO_ROOT)
        for ln, src in hits:
            print(f"{rel}:{ln}: Catch2 assertion inside a worker-thread "
                  f"lambda (thread-unsafe): {src}")
    if total:
        print(
            f"\nthread_assert_check: {total} thread-unsafe assertion(s). "
            "Record the result in an atomic/guarded value in the worker and "
            "assert on the test thread after join(); or add a trailing "
            "'// thread-assert:allow' once verified safe.",
            file=sys.stderr,
        )
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
