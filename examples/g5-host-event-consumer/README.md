# G-5 host event contract consumer

This standalone consumer exercises the merged public host boundary from an
installed SDK. It projects absolute MIDI records into a block without retiming
late or future events, derives the caller-owned MPE sidecar, and runs the
bounded graph-authored producer/consumer lane through one two-frame block and
two one-frame blocks. The two partitions must produce the same ordered trace
and counters. It also builds one public Custom-node producer/consumer edge,
checks queued boundary delivery and refusal controls, and compares routed and
reference execution.

The executable also constructs the public `SignalGraph` host-ingress adapter.
It checks exact MIDI sample tuples and feed-forward audio tuples through
partition schedules containing 1, 32, 64, 127, and 256 frame blocks. The graph
lane includes missing-edge, one-sample boundary, reversed-tie, and payload
mismatch negatives. Hosted plugin voice delivery remains outside this slice.

The processor projection control builds a public `Processor` node and compares
the routed executor with the reference walk. It checks identical MIDI/UMP/MPE
sidecars, absolute offsets, producer tie ordering, future/late/stale refusal,
fixed-capacity overflow, and a one-event-short destination refusal. The
private-header compile control below is intentionally expected to fail.

The lifecycle controls additionally exercise staging overflow, transactional
output-capacity refusal, late publication, epoch reset, queued look-ahead
delivery, and refusal of a retired graph epoch.

Build against a freshly installed SDK from the same source revision:

```sh
cmake -S examples/g5-host-event-consumer -B build-g5-host-event-consumer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PULP_SDK_PREFIX"
CMAKE_BUILD_PARALLEL_LEVEL=1 tools/ci/governed-build.sh cmake --build build-g5-host-event-consumer \
  --target pulp-g5-host-event-consumer
ctest --test-dir build-g5-host-event-consumer --output-on-failure

c++ -std=c++20 -I"$PULP_SDK_PREFIX/include" -c \
  examples/g5-host-event-consumer/private-header-negative.cpp \
  -o /tmp/g5-private-header-negative.o
# The command above must exit nonzero: private runtime headers are not shipped.
```

Expected receipt:

```text
g5-host-event-contract PASS checks=409 host_mpe_events=3 graph_events=9 graph_ingress_events=5 partitions=1,32,64,127,256 planted_negative=5
```

The `checks=378` count is the expected count from the current fixture source;
it is not a receipt until the executable is built and run against a freshly
installed SDK from the same source revision.
