# G11 live retuning: retained macOS offline receipt

Status: recipe evidence only; full G11 **PARTIAL**.

The clean source-install SDK in this packet is bound to implementation commit
`d325da30ecb79c95a915940e565203a0eee2aae4`. The configured profile and build type
are both Release, source dirtiness is false, bridge execution is false, GPU
is disabled, and optional Scala tuning is enabled. This is a local source
install, not an official distributable SDK. Subsequent evidence/documentation
commits do not change the instrument or tuning-control source. The installed
integration driver isolates SDK staging by the configure's source/profile
stamp so later source identities never overwrite an immutable SDK marker.

- [receipt.json](receipt.json): exact source/planning identity, tolerances,
  measured maxima, binaries and remaining obligations.
- [consumer.log](consumer.log): exact control-write samples, before/after phase,
  independent phase oracle, measured pitch and all 18 partition renders plus
  the Scala render.
- [live-retune.wav](live-retune.wav): 48 kHz, 24-bit stereo PCM; retuned voice
  left, unchanged sibling right. Numeric checks use float buffers before export.
- [ctest.log](ctest.log): positive plus late-write/reset/sibling/missing-write
  controls, 5/5 passed.
- [confirm-failure.log](confirm-failure.log): official baseline/broken/restored
  actual-write mutation, with observed recompilation in all three phases.
- [sdk-provenance.json](sdk-provenance.json),
  [compile-commands.json](compile-commands.json),
  [public-header-sha256.json](public-header-sha256.json), and
  [fixture-sha256.json](fixture-sha256.json): source, Release compiler flags,
  installed-only include roots and exact tested inputs.

The independent scalar sample oracle's maximum absolute error is
`1.4899876221718955e-08` against `1e-6`. Maximum circular phase error is
`1.02784447619797e-12` cycles against `1e-9`. Each rate's six partition renders
are bit-identical. The boundary at sample 8191 changes 440 to 660 Hz; sample
24577 changes 660 to 442 Hz. Each change preserves exactly the observed phase
and envelope, while the next sample increment adopts the changed frequency.
Every rendered sample and callback endpoint, both voices' lifecycles, and 28
refusal/preservation checks per render are validated.

The authoritative planning branch read was
`codex/audio-graph-roadmap-final-20261101` at
`17eef26c48b4d5479db37b15973cbbfe10532d89`, including its newest prefix,
G11 status and roadmap sections 2 and 6. The consumer uses the existing public
Processor, graph preparation, MIDI, StateStore/ParamReader, tuning provider,
VaOscillator, ADSR, pitch analyzer and WAV writer paths. No core runtime API,
scheduler, executor, tuning math, G2/G5/G6/G9 source or standing-review/admission
state was changed. The cookbook's fixed test rack is representative of a
runtime-editable instrument rack; this does not claim notation/catalog-only
authoring or serialization parity.

MTS external master, concurrent/raced realtime control, allocations/locks/I/O,
deadlines, device capture, non-Darwin consumer parity, baked/notation/serialized
parity and the remaining G11 spatial/live-sampling requirements are **NOT
CHECKED**. The host serves required gate VMs: heavy source/build suites and
diff coverage are **NOT CHECKED** locally and remain CI/light-host obligations.
The docs checker passes with repository-wide advisory warnings. Review is a
technical closeout only and is not counted standing approval.

Reproduce with a clean Release root build, then run:

```sh
cmake -DPULP_BUILD_DIR=/absolute/root/build \
  -DPULP_SOURCE_DIR=/absolute/root \
  -P examples/g11-live-retune-consumer/installed_gate.cmake
```

The root CTest registration is `g11-live-retune-installed-sdk`. A Release
configured root with tests enabled discovers it; Debug does not register this
Release-SDK evidence lane. Ninja and Ninja Multi-Config positive/negative
registration controls are retained in `registration-controls.log`. The standalone SDK driver
installs, configures, uses the governed build wrapper and runs all five cases.
The retained SHA256 manifest covers the files in this packet. Final publication
handoff additionally binds the final local/remote/API head to a fresh clean
source-install revalidation; it does not rewrite these earlier measurements as
if they were originally collected at that later commit.
