# G-4 voice-bank persistence consumer

This standalone consumer uses only the installed `Pulp::host` SDK. It exercises
control-side voice-bank save/load, canonical encode/decode bytes, edit undo/redo,
and refusal of shared or forged shared bindings. It does not create runtime
nodes, process MIDI, render audio, or claim G-6 behavior.

Build against an installed SDK from this source revision:

```sh
cmake -S examples/g4-voice-bank-consumer -B build-g4-consumer -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PULP_SDK_PREFIX"
cmake --build build-g4-consumer --target pulp-g4-voice-bank-consumer
ctest --test-dir build-g4-consumer --output-on-failure
```
