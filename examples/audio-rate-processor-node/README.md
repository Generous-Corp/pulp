# Dense AudioRate ProcessorNode

This headless example shows a native `Processor` consuming a dense, per-sample
parameter lane from Pulp's graph runtime. `AudioRateGain` declares `Gain` as an
`AudioRate` parameter, opts into dense delivery through `NodeCapabilities`, and
implements `Processor::process_block(ProcessBlock&)`.

The graph has two input channels. Channel 0 carries audio. Channel 1 is a real
graph producer for the gain lane. The runtime maps and clamps that producer into
the parameter's plain `0..1` domain and publishes the resulting borrowed span in
`EventBlock::audio_rate_modulations`. The processor multiplies each audio sample
by the corresponding gain sample, and the executable checks the result against
an independent scalar oracle.

Dense lane samples are transient DSP data. They do not update the parameter's
persistent base value in `StateStore`, and they are not added to the sparse
`ParameterEventQueue`. Hosted AU, VST3, CLAP, and LV2 plug-ins continue to use
their existing parameter-event adapters.

Build it from the Pulp source tree:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build --target pulp-audio-rate-processor-node
./build/examples/audio-rate-processor-node/pulp-audio-rate-processor-node
```

The same directory is a standalone installed-SDK consumer:

```sh
cmake -S examples/audio-rate-processor-node -B build-audio-rate-example \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_PREFIX_PATH=/path/to/pulp-sdk
cmake --build build-audio-rate-example
./build-audio-rate-example/pulp-audio-rate-processor-node
```
