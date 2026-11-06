# G-5 Wave 2 runtime transport receipt

Head: `758eb2da0f7e45ab03e72068779c1afa7889daa6`

This slice extends the admitted typed trigger runtime with a fixed 64-record
future-sample queue. `TriggerContext::emit_at_sample()` accepts an absolute
sample in the current epoch; same-sample events retain the existing
16-attempt/8-accepted budget, while future records are ordered by sample then
producer sequence and drained at the exact sample across block boundaries.
Negative coordinates, stale epochs, late records, and queue exhaustion refuse
without mutating the live graph. Reset clears the queue and audio feedback
state. Existing one-edge admission still rejects pair feedback and all
unsupported payloads; audio feedback remains owned by the existing bounded
graph pool.

## Positive and planted-negative controls

```text
./build/test/pulp-test-group-trigger-graph '[trigger]'
All tests passed (609 assertions in 27 test cases)

ctest --test-dir build -R 'trigger-runtime-public-consumer|trigger-runtime$' --output-on-failure
100% tests passed, 0 tests failed out of 1
```

The new positive case schedules samples 5 and 3 in reverse publication order,
renders three uneven blocks, and observes samples 3 then 5. The negative case
covers negative coordinates, the per-sample attempt fuse, and late scheduled
records after a cursor advance. The production branch is mutation-tested:

```text
bash tools/scripts/confirm_failure.sh \
  --file core/host/src/trigger_graph.cpp \
  --break "perl -0pi -e 's/pending_size == pending_capacity/pending_size == 0/'" \
  --build-dir build --target pulp-test-group-trigger-graph \
  --test "./build/test/pulp-test-group-trigger-graph 'Typed transport retains future samples across block boundaries'"
CONFIRMED — the test passes with the fix and fails without it.
```

This receipt proves the bounded framework trigger transport only. Host absolute
timestamp ingress, arbitrary graph-authored callback registration, plugin
parity, and hard-realtime qualification remain outside this slice.
