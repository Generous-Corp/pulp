# G-5 host event contract consumer

This standalone consumer exercises the merged public host boundary from an
installed SDK. It projects absolute MIDI records into a block without retiming
late or future events, derives the caller-owned MPE sidecar, and runs the
bounded graph-authored producer/consumer lane through one two-frame block and
two one-frame blocks. The two partitions must produce the same ordered trace
and counters.

The executable includes planted negatives for stale and duplicate host records,
late records, transactional output-capacity refusal, stale graph epochs,
accepted-event overflow, and the per-sample attempt limit. It does not claim
SignalGraph wiring, adapter timestamp projection, or graph-authored voice
delivery; those remain separate runtime contracts.

Build against a freshly installed SDK from the same source revision:

```sh
cmake -S examples/g5-host-event-consumer -B build-g5-host-event-consumer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PULP_SDK_PREFIX"
CMAKE_BUILD_PARALLEL_LEVEL=1 tools/ci/governed-build.sh cmake --build build-g5-host-event-consumer \
  --target pulp-g5-host-event-consumer
ctest --test-dir build-g5-host-event-consumer --output-on-failure
```

Expected receipt:

```text
g5-host-event-contract PASS checks=79 host_mpe_events=3 graph_events=9 planted_negative=1
```
