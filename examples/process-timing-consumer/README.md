# Process timing consumer

This standalone consumer exercises the neutral adapter timing facts from an
installed SDK. A host binding reads only `ProcessContext::timing` and the
block-relative MIDI offsets an adapter delivers, advances one
`pulp::host::HostBlockTimeline`, and stages each event in a
`HostEventIngress` at its epoch-local absolute sample. The contexts are built
with the public `format::clap_process_timing()` and
`format::lv2_process_timing()` builders the adapters use, so no plugin SDK is
needed.

It checks activation, a playing seek, stopped transport and a relocation while
stopped (neither is a seek), skipped host blocks (a gap in the same epoch), a
lost sample counter (a new epoch), refusal of offsets outside the block, a
retired-epoch record refused by both the timeline and the ingress, identical
absolute samples for three block partitions, LV2 half-speed playback that is
not read as a seek, and an oversized block that is refused whole and accepted
as `host_block_chunk()` chunks.

The same source is built in-tree as `process-timing-public-consumer`. To check
it against an installed SDK from the same source revision:

```sh
cmake -S examples/process-timing-consumer -B build-process-timing-consumer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PULP_SDK_PREFIX"
CMAKE_BUILD_PARALLEL_LEVEL=1 tools/ci/governed-build.sh cmake --build build-process-timing-consumer \
  --target pulp-process-timing-consumer
ctest --test-dir build-process-timing-consumer --output-on-failure
```

Expected output: `process-timing-consumer PASS checks=34`.
