# P1 event admission public consumer

This installed-SDK consumer is the bounded admission fixture for P1. It
exercises MIDI and trigger public seams across two block partitions; parameter
and voice checks remain standalone:

* absolute MIDI host ingress, stable producer ordering, stale-epoch and late
  refusal;
* standalone fixed-capacity parameter-queue sorting and overflow refusal
  (graph/mailbox projection is not checked);
* the existing trigger graph through two blocks, including absolute samples,
  sequence, epoch, and reset;
* a standalone test-owned voice-intent adapter that pairs the existing G-4
  authored voice identity with the existing MPE allocator (graph voice
  projection is not checked). It adds no runtime queue or graph API.

The fixture is qualified only when built from a complete installed SDK and the
positive executable exits zero. Missing SDK support modules are `NOT_CHECKED`,
not a qualification claim. The five planted negatives are stale MIDI epoch,
late MIDI, duplicate MIDI sequence, parameter overflow, and stale voice
generation. The positive control proves two-block MIDI and trigger behavior plus
partition-invariant MIDI output. Parameter and voice projection remain planned,
not checked by this fixture.

```sh
cmake -S examples/p1-event-admission-consumer -B build-p1-event-admission \
  -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PULP_SDK_PREFIX"
CMAKE_BUILD_PARALLEL_LEVEL=1 tools/ci/governed-build.sh cmake --build \
  build-p1-event-admission --target pulp-p1-event-admission-consumer
ctest --test-dir build-p1-event-admission --output-on-failure
```
