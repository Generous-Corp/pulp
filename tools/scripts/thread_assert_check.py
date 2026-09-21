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

WHAT THIS SCAN CATCHES — and, just as importantly, WHAT IT DOES NOT
===================================================================
This is a best-effort LEXICAL scan. It has no type information and does not
follow calls into named functions. Read a clean run as "none of the shapes
below are present", never as "no thread-unsafe assertion exists". The list is
explicit so the next reader does not mistake silence for proof.

CAUGHT (each has a fixture in tools/scripts/test_thread_assert_check.py):

  1. A lambda handed directly to a thread-spawning construct:
         std::thread t([&]{ REQUIRE(x); });
         std::jthread t([&](std::stop_token){ REQUIRE(x); });
         std::async(std::launch::async, [&]{ REQUIRE(x); });

  2. A lambda pushed into a CONTAINER of threads/futures — the shape that
     actually shipped into test_present_timing_mac.cpp and that the original
     narrow scan could not see, because the `std::thread` token sits in the
     DECLARATION and never appears on the emplace line:
         std::vector<std::thread> threads;
         threads.emplace_back([&]{ REQUIRE(x); });      // flagged
         threads.push_back(std::thread([&]{ ... }));    // flagged
         std::array<std::jthread, 4> workers;           // also tracked
     Recognised containers: vector / array / deque / list / forward_list
     (optionally std::pmr::), holding std::thread, std::jthread, std::future,
     std::shared_future, or std::packaged_task.

  3. A lambda bound to a variable first and spawned later:
         auto body = [&]{ REQUIRE(x); };
         std::thread t(body);                            // flagged
         threads.emplace_back(body);                     // flagged
     Recognised bindings: `auto` / `auto&&` / `const auto` / `std::function<>`.

  4. Transitive one-name-at-a-time indirection: a flagged thread body that
     names ANOTHER lambda bound in the same translation unit pulls that
     lambda's body in too, repeatedly, until the set stops growing.

NOT CAUGHT (known holes — a clean exit says nothing about these):

  * An assertion inside a named free/member/static function that a thread body
    calls. Following that needs a call graph, which needs a parser.
  * A lambda reached through std::function stored in a member/struct field, a
    map, or a vector of callables, or returned from a factory function.
  * Threads spawned by a helper the test calls (a fixture's `run_on_pool(...)`,
    a thread pool, a dispatch queue). Only the literal constructs above are
    recognised as spawn sites — notably NOT dispatch_async / pthread_create /
    parallel algorithms.
  * A macro that expands to an assertion under a different spelling.
  * An assertion on a thread the test did not spawn (an audio callback, a
    platform completion handler, a signal handler).

Verified-safe lines can be suppressed with a trailing `// thread-assert:allow`
comment. (INFO/CAPTURE message scopes are also thread-unsafe but are not
flagged here — the assertion macros are the demonstrated corruption vector.)

`--wide` runs a deliberately over-broad diagnostic pass — every assertion in
ANY lambda body in a translation unit that spawns a thread — and prints what it
finds WITHOUT failing. It is a review aid for auditing the holes above, not a
gate: most of its output is helper lambdas that run on the test thread.
"""

from __future__ import annotations

import argparse
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

# A container whose ELEMENT type is a thread/future. The `std::thread` token
# lives here, in the declaration — never on the emplace_back line that actually
# installs the body, which is why a per-line scan cannot see this shape.
CONTAINER_RE = re.compile(
    r"\b(?:std::)?(?:pmr::)?(?:vector|array|deque|list|forward_list)\s*<")
THREAD_ELEMENT_RE = re.compile(
    r"\bstd::(?:thread|jthread|shared_future|future|packaged_task)\b")
# `threads.emplace_back(...)` / `.push_back(...)` / `.emplace(...)` — the call
# that installs a body into a tracked container.
INSTALL_RE = re.compile(r"\b(\w+)\s*\.\s*(?:emplace_back|push_back|emplace)\s*\(")
# `auto body = [...]` — a lambda bound to a name, spawned on a later line.
BINDING_RE = re.compile(
    r"(?:^|[;{}(,])\s*(?:const\s+)?(?:auto\s*&&?|auto|std::function\s*<[^;{}]*>)"
    r"\s+(\w+)\s*=\s*$")
IDENT_RE = re.compile(r"\b([A-Za-z_]\w*)\b")
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


def _match_pair(code: str, open_idx: int, opener: str, closer: str) -> int:
    """Index of the closer matching the opener at `open_idx`, or -1."""
    depth = 0
    for j in range(open_idx, len(code)):
        c = code[j]
        if c == opener:
            depth += 1
        elif c == closer:
            depth -= 1
            if depth == 0:
                return j
    return -1


# Tokens legal between a lambda's `]` and its `{`: parameter list (handled as a
# balanced group), `mutable`, `noexcept`, `constexpr`, a trailing return type,
# and an explicit template parameter list.
_TRAILER_OK = set(" \t\r\n->&*:<>,.~_")


def find_lambdas(code: str) -> list[tuple[int, int, int, int]]:
    """Every lambda literal in `code` as (cap_open, cap_close, body_open,
    body_close). `code` must already be blank_noncode()'d."""
    out: list[tuple[int, int, int, int]] = []
    n = len(code)
    i = 0
    while i < n:
        if code[i] != "[":
            i += 1
            continue
        # `[[nodiscard]]`-style attributes are not lambdas.
        if i + 1 < n and code[i + 1] == "[":
            i += 2
            continue
        if i and code[i - 1] == "[":
            i += 1
            continue
        # A subscript `v[0]` / `f()[0]` is not a lambda introducer.
        k = i - 1
        while k >= 0 and code[k] in " \t\r\n":
            k -= 1
        prev = code[k] if k >= 0 else ""
        if prev.isalnum() or prev in "_)]":
            i += 1
            continue
        cap_close = _match_pair(code, i, "[", "]")
        if cap_close < 0:
            i += 1
            continue
        j = cap_close + 1
        body_open = -1
        while j < n:
            c = code[j]
            if c == "(":
                nxt = _match_pair(code, j, "(", ")")
                if nxt < 0:
                    break
                j = nxt + 1
                continue
            if c == "{":
                body_open = j
                break
            if c.isalnum() or c in _TRAILER_OK:
                j += 1
                continue
            break
        if body_open < 0:
            i += 1
            continue
        body_close = _match_pair(code, body_open, "{", "}")
        if body_close < 0:
            i += 1
            continue
        out.append((i, cap_close, body_open, body_close))
        i += 1
    return out


def statement_span(code: str, start: int,
                   by_cap: dict[int, tuple[int, int, int, int]]) -> int:
    """End offset of the statement beginning at `start`, stepping OVER any
    lambda body so a `;` inside one does not end the statement early."""
    n = len(code)
    i = start
    while i < n:
        lam = by_cap.get(i)
        if lam is not None:
            i = lam[3] + 1
            continue
        c = code[i]
        if c == ";":
            return i
        if c == "{":
            # A real block (`if (...) {`), not a lambda body: statement over.
            return i
        i += 1
    return n


def thread_container_names(code: str) -> set[str]:
    """Identifiers declared as a container of threads/futures."""
    names: set[str] = set()
    for m in CONTAINER_RE.finditer(code):
        lt = m.end() - 1
        gt = _match_pair(code, lt, "<", ">")
        if gt < 0:
            continue
        if not THREAD_ELEMENT_RE.search(code[lt + 1:gt]):
            continue
        tail = re.match(r"\s*(\w+)", code[gt + 1:])
        if tail:
            names.add(tail.group(1))
        # `auto workers = std::vector<std::thread>{};`
        head = BINDING_RE.search(code[max(0, m.start() - 200):m.start()])
        if head:
            names.add(head.group(1))
    return names


def lambda_bindings(code: str,
                    lambdas: list[tuple[int, int, int, int]]) -> dict[str, tuple[int, int, int, int]]:
    """Map `auto name = [..]{..}` bindings to their lambda."""
    out: dict[str, tuple[int, int, int, int]] = {}
    for lam in lambdas:
        cap_open = lam[0]
        m = BINDING_RE.search(code[max(0, cap_open - 240):cap_open])
        if m:
            out[m.group(1)] = lam
    return out


def _idents(code: str, lo: int, hi: int) -> set[str]:
    return {m.group(1) for m in IDENT_RE.finditer(code, lo, hi)}


def thread_body_lambdas(code: str) -> dict[tuple[int, int, int, int], str]:
    """Every lambda whose body can run on a worker thread, mapped to the shape
    that identified it."""
    lambdas = find_lambdas(code)
    by_cap = {lam[0]: lam for lam in lambdas}
    bindings = lambda_bindings(code, lambdas)
    containers = thread_container_names(code)

    found: dict[tuple[int, int, int, int], str] = {}
    queue: list[tuple[tuple[int, int, int, int], str]] = []

    def spawn_site(start: int, shape: str) -> None:
        end = statement_span(code, start, by_cap)
        for lam in lambdas:
            if start <= lam[0] < end:
                queue.append((lam, shape))
        for name in _idents(code, start, end):
            lam = bindings.get(name)
            if lam is not None:
                queue.append((lam, f"{shape} via named lambda `{name}`"))

    for m in THREAD_RE.finditer(code):
        spawn_site(m.end(), "std::thread/jthread/async")
    for m in INSTALL_RE.finditer(code):
        if m.group(1) in containers:
            spawn_site(m.end() - 1, f"container of threads `{m.group(1)}`")

    while queue:
        lam, shape = queue.pop()
        if lam in found:
            continue
        found[lam] = shape
        # A thread body that names another lambda runs that lambda too.
        for name in _idents(code, lam[2], lam[3]):
            nxt = bindings.get(name)
            if nxt is not None and nxt not in found:
                queue.append((nxt, f"{shape} -> named lambda `{name}`"))
    return found


def line_of(text: str, off: int) -> int:
    return text.count("\n", 0, off) + 1


def scan_file(path: Path, wide: bool = False) -> list[tuple[int, str, str]]:
    """Return (line, source, shape) for each thread-unsafe assertion."""
    raw = path.read_text(encoding="utf-8", errors="replace")
    code = blank_noncode(raw)
    raw_lines = raw.splitlines()

    if wide:
        if not THREAD_RE.search(code) and not thread_container_names(code):
            return []
        bodies = {lam: "wide: any lambda in a thread-spawning TU"
                  for lam in find_lambdas(code)}
    else:
        bodies = thread_body_lambdas(code)

    hits: dict[int, tuple[int, str, str]] = {}
    for (_, _, open_idx, close_idx), shape in bodies.items():
        for am in ASSERT_RE.finditer(code, open_idx, close_idx):
            ln = line_of(raw, am.start())
            src = raw_lines[ln - 1] if ln - 1 < len(raw_lines) else ""
            if SUPPRESS in src:
                continue
            # An assertion inside nested lambdas is reported once, under the
            # outermost shape that reached it.
            if am.start() not in hits:
                hits[am.start()] = (ln, src.strip(), shape)
    return [hits[k] for k in sorted(hits)]


def run(wide: bool) -> int:
    if not TEST_DIR.is_dir():
        print(f"thread_assert_check: no test dir at {TEST_DIR}", file=sys.stderr)
        return 0
    files = sorted(TEST_DIR.rglob("*.cpp")) + sorted(TEST_DIR.rglob("*.mm"))
    total = 0
    for path in files:
        hits = scan_file(path, wide=wide)
        if not hits:
            continue
        total += len(hits)
        rel = path.relative_to(REPO_ROOT)
        for ln, src, shape in hits:
            print(f"{rel}:{ln}: Catch2 assertion inside a worker-thread "
                  f"lambda (thread-unsafe) [{shape}]: {src}")
    if wide:
        print(f"\nthread_assert_check --wide: {total} assertion(s) in some "
              "lambda in a thread-spawning translation unit. This pass is "
              "DIAGNOSTIC and always exits 0: most hits are helper lambdas "
              "that run on the test thread.")
        return 0
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


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--wide", action="store_true",
                    help="diagnostic over-broad pass; never fails")
    return run(ap.parse_args().wide)


if __name__ == "__main__":
    sys.exit(main())
