# Paced shared-memory convolution probe

This source-build diagnostic drives the public `GpuAudioTransport` and
`GpuConvolver` with a stereo, 257-tap convolution. The transport's worker owns
GPU submission and completion service. A separate thread supplies paced audio
blocks and never calls `pump()`. The probe checks the delivered stream against
independent direct-double convolution after the timed interval, including
latency-aligned CPU fallback.

Configure a Release Apple Silicon build with the pinned provider and these
options:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release -DCMAKE_OSX_ARCHITECTURES=arm64 \
  -DPULP_BUILD_TESTS=ON -DPULP_ENABLE_GPU=ON \
  -DPULP_GPU_AUDIO_EXACT_PROVIDER_PROOF=ON \
  -DPULP_GPU_AUDIO_ENABLE_EXPERIMENTAL_SHARED_IO_CONVOLVER=ON
tools/ci/governed-build.sh cmake --build build \
  --target pulp-gpu-shared-io-paced-convolution-probe
ctest --test-dir build -R '^pulp-gpu-shared-io-paced-convolution-probe$' \
  --output-on-failure
```

Run a bounded diagnostic, choosing a new output directory:

```sh
build/test/pulp-gpu-shared-io-paced-convolution-probe \
  --frames=32 --lead=2 --blocks=4096 --warmup=64 \
  --output-dir=/absolute/path/to/new-capture
```

Supported frames are 32, 64, and 128; lead is 1, 2, 4, or 8 blocks. The sample
rate is 48 kHz and physical provider slots remain two. `--wake-on-write` enables
the transport's existing semaphore notification instead of its default polling
worker. Completion service remains the provider's default `ProcessEvents`
policy. The lower-level provider probe can compare this with
`--completion-policy=wait-any` or `--completion-policy=timed-wait-any`; those
policies retain each submission's Dawn `Future` handles and wait on the
serialized non-realtime dispatcher. `timed-wait-any` requests Dawn's
`TimedWaitAny` instance feature and accepts `--completion-wait-ns=N` as a
dispatcher wait bound. Pop-error-scope and device-lost callbacks still use
`AllowProcessEvents` and are explicitly pumped before terminal completion is
published. These policies keep Dawn calls off the audio callback and do not
promise hard realtime behavior or GPU scheduling priority.
The maximum is 20,000 measured blocks per invocation.

`receipt.json` contains configuration, numerical failures, callback overruns,
late callback starts, and the transport's miss-counter delta. `blocks.csv`
preserves every callback position, source position, scheduled time, observed
callback begin/end, miss-counter delta, and maximum numerical error. Warmup and
initial pipeline priming are retained but excluded from measured-block counts.
No file output or numerical-oracle work occurs during the paced interval.

Exit 0 means the stream passed the numerical oracle and the worker made GPU
progress. It does not mean that every deadline was met. Exit 1 is a completed
failed check; exit 2 is invalid configuration, unavailable shared provider, or
an execution/artifact error. `--negative-control` corrupts one measured output
sample after processing; the verifier requires exactly one failed block and
exit 1. Unavailability cannot satisfy that negative control.

This is a simulated callback driven by a non-realtime thread, not an audio
device or host acceptance test. Callback begin/end are external CPU clock
observations, not GPU timestamps. Miss-counter deltas are not a substitute for
the full per-block terminal/disposition trace. Retain source revision, binary
and provider hashes, build flags, machine/load/thermal context alongside these
artifacts before making comparisons. The receipt deliberately leaves the
performance verdict unassigned; matched staged/shared trials and host tests
remain separate requirements.
