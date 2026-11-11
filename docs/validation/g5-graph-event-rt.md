# G-5 graph event callback safety fixture

The test in `test/fixtures/g5_graph_event_rt_cases.inc` drives the public graph event writer and reader through one Custom producer and one Custom consumer. It uses the canonical routed executor and the reference serial executor, with 64, 128, 256, and 4096 frame calls at 48 kHz. Preparation and graph construction happen before the measured callback region. The positive fixture stores event tuples and audio in fixed arrays, checks exact tie order and sample positions, compares both backends and all partitions, and exercises a reset with a queued event.

`RtAllocationProbe` counts C++ `new` calls on the callback thread. `RtContractProbe` observes explicit lock and blocking controls in that region. The two deterministic Release cases passed 65,990 assertions (65,822 partition/backend assertions and 168 reset/control assertions) with zero counted allocations or bytes, zero explicit lock or blocking events, seven exact events per render, and the queued event retired by reset through its delivery window. These counters are evidence for this public callback fixture, not a whole-graph proof that every possible C allocation, lock, wait, or I/O site is instrumented.

The repaired focused cases also passed all 65,990 assertions under macOS AddressSanitizer in a governed Release build. The restored production source and fixture were explicitly recompiled before that run. This repair changes tests and validation documentation only; no runtime, public headers, installed SDK, or installed-consumer surface changed, so a new installed-consumer qualification is not affected.

The three compiled `confirm_failure.sh` controls in `test/fixtures/g5_rt_escape_mutator.py` put an actual `new[]`, `std::mutex::lock`, or `write(1, ...)` in the Custom producer callback. Each returned baseline/broken/restored exits `0/42/0` and `CONFIRMED`. They prove the assertions detect these planted escapes. The earlier G-5 qualification bundle separately contains six mutations of the production event contract header.

The reset regression runs the same one-shot producer with and without a reset on each serial backend. It queues payload 17 at absolute sample 2 during the first one-frame callback. Four callbacks cover `[0,1), [0,1), [1,2), [2,3)` with reset, and `[0,1), [1,2), [2,3), [3,4)` without reset. Every callback runs inside the allocation and contract probes. The reset path requires zero deliveries and silent output throughout; the no-reset control requires exactly one sample-2/payload-17 event, audio value 17 only in its delivery callback, and no duplicate. Both paths require exactly four producer calls and no rejected emissions or malformed events.

A targeted compiled mutation bypasses only the graph-event discontinuity reset condition in `core/host/src/signal_graph_process.cpp`. `confirm_failure.sh` reported `CONFIRMED`: the repaired regression passed before mutation, failed six delivery/audio assertions while bypassed (three per serial backend), and passed all 168 assertions after restoration. The broken reset scenarios delivered the old payload at callback 2; the otherwise identical no-reset controls still passed. The production source was restored byte-for-byte (SHA-256 `1ebc5493bd98222536e67cecf855a355817dac12df931d44e59747e381f17c39`). Compilation or unrelated CI errors were not used as negative evidence.

Reproduce that targeted mutation only with the restoring harness, from a clean production source:

```sh
tools/scripts/confirm_failure.sh \
  --file core/host/src/signal_graph_process.cpp \
  --break 'python3 test/fixtures/g5_rt_escape_mutator.py reset' \
  --build-dir build --target pulp-test-group-trigger-graph --jobs 4 \
  --test "build/test/pulp-test-group-trigger-graph 'G5 callback reset retires a queued graph event without allocating' --reporter compact" \
  --keep-logs
```

The optional `G5 graph event callback paced measurement` case runs 128 callbacks per frame size and backend on a separate paced thread. It prints p99 callback wall time, p99 wake jitter, and deadline misses. It is hidden from default CTest. On the observed 2026-11-10 host, load rose from `54.70/38.54/37.61` to `61.82/55.33/46.35` during the original qualification run; pacing was not rerun for this reset-oracle repair. At 64 frames the reference and routed runs missed 36 and 19 deadlines; other sizes also missed deadlines. This loaded-host sample is diagnostic and does not qualify the callback's realtime timing.

Reproduce the focused positive cases after a governed Release test build:

```sh
build/test/pulp-test-group-trigger-graph 'G5 graph event callbacks keep fixed storage across both serial backends' --reporter compact
build/test/pulp-test-group-trigger-graph 'G5 callback reset retires a queued graph event without allocating' --reporter compact
```

Run the paced case only after a quiet-host admission and record host load, 128-call exploratory output, and a longer qualification run separately:

```sh
build/test/pulp-test-group-trigger-graph 'G5 graph event callback paced measurement' --reporter compact
```

G-5 realtime gate 6 remains open. The roadmap's qualification bar is 44.1, 48, and 96 kHz at 32, 64, 128, and 256 frames in Release; p99 callback time below 70% of budget, p99 jitter below 10% of one frame, and zero misses in 100,000 paced callbacks. It also requires no allocations, locks, waits or I/O in the callback, with exact PDC and activation latency, WCET and memory per fixture. This fixture covers one 48 kHz event path, uses explicit lock/block probes, and has no quiet-host 100,000-call run. RTSan and other platform sanitizer coverage are NOT CHECKED here.
