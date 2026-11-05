# Typed trigger admission foundation

This public consumer checks the production graph planner's release-build lane
admission. It does not render triggers: typed triggers remain explicitly refused
until the prepared producer/consumer runtime pair is implemented. Existing MIDI
Event and automation feedforward edges and plain-audio feedback remain admitted.

Build against a freshly installed SDK from this source revision:

```sh
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH="$PULP_SDK"
cmake --build build --parallel 1
ctest --test-dir build --output-on-failure
```
