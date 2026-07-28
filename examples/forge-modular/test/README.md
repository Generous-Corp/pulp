# Forge Modular tests

## `block_size_parity.cpp` — the cross-target gate

The DAW products and the Rack modules run the same DSP at different block
sizes: a host hands Pulp 64–4096 frames, Rack calls `process()` once per
sample. This renders one baked graph both ways and compares.

It asserts two things, because the truth is asymmetric:

1. A **feedback-free** graph is **bit-identical** at block 1 and 64. Drift here
   is a real executor or bake bug.
2. A graph with a **feedback edge** legitimately **differs**, because
   `SignalGraph::connect_feedback` is a *one-block* delay whose delay time
   therefore scales with block size. The test pins that difference so it cannot
   change silently, and so an unexpected *match* also fails.

Build against a configured Pulp build:

```sh
R=<pulp-worktree>; B=$R/build
LIBS=$(find $B/core -name 'libpulp-*.a' | grep -v install-layout \
       | grep -vE 'render|canvas|inspect|gpu-audio|perfetto|view|osc|ship|native-components|bundled-fonts|annotated-capture|audio-analysis|audio-test-support')
M="$B/_deps/mbedtls-build/library/libmbedtls.a $B/_deps/mbedtls-build/library/libmbedx509.a $B/_deps/mbedtls-build/library/libmbedcrypto.a"
clang++ -std=c++20 -O2 block_size_parity.cpp -o /tmp/parity \
  $(for d in host format audio midi runtime state signal platform events graph dsl; do echo -n "-I$R/core/$d/include "; done) \
  -I$B/_deps/choc-src $LIBS $LIBS $B/libvst3-sdk.a $B/libausdk.a $M \
  -framework CoreFoundation -framework CoreAudio -framework AudioToolbox \
  -framework Foundation -framework Accelerate -framework CoreMIDI \
  -framework AVFoundation -framework Security -framework AppKit -framework CoreServices
/tmp/parity
```
