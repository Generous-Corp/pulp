# Testing & Hardening

Pulp ships three instrumentation modes that any developer can run locally
and CI runs selectively. All are **opt-in** — Debug/Release/RelWithDebInfo
builds are unaffected.

| Mode | Flag | Purpose | CI status |
|---|---|---|---|
| **Coverage** | `-DPULP_ENABLE_COVERAGE=ON` | Measure what the test suite actually exercises | Non-blocking artifact |
| **AddressSanitizer + UBSan** | `-DPULP_SANITIZER=address` | Heap/stack overflow, use-after-free, UB | Blocking (when enabled) |
| **ThreadSanitizer** | `-DPULP_SANITIZER=thread` | Data races, lock ordering | Advisory |
| UndefinedBehaviorSanitizer | `-DPULP_SANITIZER=undefined` | Signed overflow, null deref, alignment | On-demand (`scripts/run_ubsan.sh`) |
| MemorySanitizer | `-DPULP_SANITIZER=memory` | Uninitialized reads (Clang only; requires all-deps instrumented) | Manual |
| RealtimeSanitizer | `-DPULP_SANITIZER=realtime` | Malloc / lock / syscall inside an RT section (Clang 18+) | Manual |

Coverage and sanitizers are **mutually exclusive**. Use separate build
directories (`build-coverage/`, `build-asan/`, etc.) so they coexist.

## Coverage

### Run locally

```bash
scripts/run_coverage.sh
```

Produces:
- `build-coverage/coverage/index.html` — interactive drilldown, per file.
- `build-coverage/coverage/summary.txt` — top-level table you can scan in
  a terminal.

### Interpret results

```
Filename               Regions   Missed  Cover%   Functions  Missed  Cover%   Lines      Missed  Cover%
core/runtime/...        12345      1234   90.0%        456      45    90.1%      23456    2345    90.0%
```

- **Regions** — branches + basic blocks. Best proxy for "did a test
  actually step through this code."
- **Lines** — executed at least once. Easier to reason about but
  over-counts multi-statement lines.
- **Functions** — % of declared functions that ran at least once.

What to do with the numbers:
1. Pick a subsystem under `#290` (platform / host / format / runtime /
   view / osc / dsl).
2. Sort the summary by Regions Cover% ascending.
3. Write tests for the top 3 files with lowest region coverage.
4. Open a PR that raises the column by a few points. No hard threshold —
   the direction matters, not the absolute number.

### What coverage won't tell you

- Concurrency correctness. That's TSan.
- Memory safety. That's ASan.
- Whether a test has real assertions or just exercises code without
  checking anything. Coverage + a passing test count is necessary but
  not sufficient.

### CI

`coverage.yml` (non-blocking) uploads the HTML as a PR artifact on every
pull request. Main-branch runs additionally push a JSON summary to
`docs/coverage-history.json` so trends can be charted over time.

## AddressSanitizer + UBSan

### Run locally

```bash
scripts/run_asan.sh
```

Any ASan or UBSan report stops the test run with a non-zero exit.

### Interpret a report

A typical ASan trace:

```
=================================================================
==12345==ERROR: AddressSanitizer: heap-use-after-free on address 0x…
READ of size 4 at 0x… thread T0
    #0 0x… in pulp::view::View::layout() core/view/src/view.cpp:123
    #1 0x… in pulp::view::WindowHost::repaint() core/view/src/window_host.cpp:456
    ...
0x… is located 8 bytes inside of 256-byte region [0x…,0x…)
freed by thread T0 here:
    #0 0x… in operator delete(void*) ...
    #1 0x… in pulp::view::View::~View() core/view/src/view.cpp:77
```

Read top-down:
1. **ERROR line** → the bug class (heap-use-after-free / stack-overflow /
   global-buffer-overflow / …).
2. **READ/WRITE of size X at 0x…** → exact address + access type.
3. **thread TN** → which thread saw the bug.
4. The stack trace of the current access.
5. The stack trace of the previous alloc/free for the same region — the
   common case is this is where the lifetime bug actually is.

Common fixes:
- Use-after-free → check object lifetime; make sure destructors don't
  leave raw pointers dangling.
- Heap-buffer-overflow → review index arithmetic + the size that was
  passed in.
- Stack-buffer-overflow → usually a fixed-size array + off-by-one.

### CI

`sanitizers.yml` runs ASan + UBSan on `pull_request` and blocks on any
report. Runners: Linux + macOS.

## ThreadSanitizer

### Run locally

```bash
scripts/run_tsan.sh
```

### Interpret a race report

```
WARNING: ThreadSanitizer: data race (pid=12345)
  Write of size 4 at 0x… by thread T1:
    #0 pulp::state::StateStore::set_param() ...
    #1 pulp::format::Processor::process() ...
  Previous read of size 4 at 0x… by main thread:
    #0 pulp::view::ParamBinding::poll() ...
```

The pattern to match: write on one thread, unsynchronized read/write on
another. Common fixes:
- Atomic + relaxed ordering if it's a single independent value.
- `SeqLock` if it's a coherent multi-field read (see `core/runtime`).
- `TripleBuffer` if it's large and read-latest.
- `SpscQueue` if it's ordered events.

### Known-safe patterns (suppressions)

TSan can flag known-safe patterns like CHOC's SPSC FIFO or atomic fences
we've deliberately relaxed. Add the offending trace signature to
`tools/tsan-suppressions.txt` (one per line, see TSan docs for format).
Every entry needs a comment explaining *why* it's safe — otherwise it's
a silent bug.

## Realtime Safety (RTSan)

RealtimeSanitizer flags allocation, mutex acquisition, and syscalls
inside functions annotated with `[[clang::nonblocking]]`. Requires
upstream Clang 18+.

On Linux:

```bash
cmake -S . -B build-rtsan -DPULP_SANITIZER=realtime
cmake --build build-rtsan
ctest --test-dir build-rtsan
```

Currently annotated: the audio-thread `process()` paths in each format
adapter. If RTSan reports an allocation in there, it's a real RT-safety
bug (see #307 Codex P1 → PR #315 for a representative case).

## Relation to #290

`#290` is the test-coverage hardening initiative for `platform`, `host`,
`format`, `runtime`, `view`, `osc`, and `dsl`. This doc describes the
tooling; #290 is the ongoing effort to turn red numbers into green ones.
Pick a subsystem, look at `summary.txt`, write tests, submit a PR.

## When should you run which?

- **Before opening a PR that changes non-trivial logic** → at minimum
  compile + run your targeted tests under ASan. Ten-second overhead,
  catches the 80% of memory bugs that would otherwise wait for CI.
- **Before opening a PR that touches threading primitives** → TSan.
- **Monthly, or before a release** → coverage report on main. Check
  which subsystems regressed and write tests.
- **When RT-safety is on the line** (audio-thread code, lock-free
  containers) → RTSan on Linux locally; TSan in CI.
