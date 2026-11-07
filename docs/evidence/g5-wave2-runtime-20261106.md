# G-5 Wave 2 runtime transport receipt

Head: `a9f5ae6389bc02052319d89d96270693419b684b`

This slice preserves the admitted current-sample typed trigger runtime.
`TriggerContext::emit_at_sample()` accepts an absolute sample in the current
epoch only; future targets are refused with `Future` because no scheduler or
cross-block queue is admitted by the G-5 event transport contract. Same-sample
events retain the existing 16-attempt/8-accepted budget. Negative coordinates,
stale epochs, late records, and over-capacity publication refuse without
mutating the live graph. Existing one-edge admission still rejects pair
feedback and all unsupported payloads; audio feedback remains owned by the
existing bounded graph pool.

## Positive and planted-negative controls

```text
./build/test/pulp-test-group-trigger-graph '[trigger]'
All tests passed (587 assertions in 26 test cases)

ctest --test-dir build -R 'trigger-runtime-public-consumer|trigger-runtime$' --output-on-failure
100% tests passed, 0 tests failed out of 1
```

The negative control publishes nine records at one sample, proving exactly eight
are delivered and the ninth returns `Overflow`; it also covers negative, future,
and late coordinates.

The cap has mutation evidence:

```text
bash tools/scripts/confirm_failure.sh \
  --file core/host/src/trigger_graph.cpp \
  --break "perl -0pi -e 's/counters\.accepted == 8/counters.accepted == 0/'" \
  --build-dir build --target pulp-test-group-trigger-graph \
  --test "./build/test/pulp-test-group-trigger-graph 'Typed transport rejects future targets and caps one sample'"
CONFIRMED — the test passes with the fix and fails without it.
```

This receipt proves the bounded framework trigger transport only. Host absolute
timestamp ingress, arbitrary graph-authored callback registration, plugin
parity, and hard-realtime qualification remain outside this slice.
