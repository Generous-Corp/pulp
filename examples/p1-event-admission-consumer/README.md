# P1 event admission public consumer

This installed-SDK consumer is the bounded admission fixture for P1. It
exercises existing public seams across two block partitions:

* absolute MIDI host ingress, stable producer ordering, stale-epoch and late
  refusal;
* the fixed-capacity parameter queue, stable sorting, and overflow refusal;
* the existing trigger graph through two blocks, including absolute samples,
  sequence, epoch, and reset;
* a test-owned voice-intent adapter that pairs the existing G-4 authored voice
  identity with the existing MPE allocator. It adds no runtime queue or graph
  API.

The fixture is qualified only when built from a complete installed SDK and the
positive executable exits zero. Missing SDK support modules are `NOT_CHECKED`,
not a qualification claim. The five planted negatives are stale MIDI epoch,
late MIDI, parameter overflow, stale voice generation, and reset/sequence
boundary misuse.

```sh
cmake -S examples/p1-event-admission-consumer -B build-p1-event-admission \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PULP_SDK_PREFIX"
CMAKE_BUILD_PARALLEL_LEVEL=1 tools/ci/governed-build.sh cmake --build \
  build-p1-event-admission --target pulp-p1-event-admission-consumer
ctest --test-dir build-p1-event-admission --output-on-failure
```
