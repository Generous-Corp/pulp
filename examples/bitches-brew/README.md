# Bitches Brew

Control-voltage plug-ins. They emit DC and audio-rate signals meant for the CV
inputs of an analog modular synth, reached through a **DC-coupled** audio
interface (optionally via ADAT to a DC-coupled expander).

MIT, like the rest of Pulp. No registration, no activation, no phone-home.

## Why a plug-in can be a voltage source

An audio interface converts sample values to a voltage. Most interfaces
AC-couple their outputs, high-passing away anything near 0 Hz — so a constant
sample value decays to nothing at the jack. A **DC-coupled** output does not,
which means a steady sample value is a steady voltage, and a plug-in that writes
`0.5` to every sample is a plug-in that holds half of full scale at the jack.

That is the whole trick, and it is also why these plug-ins are unusually strict
about signal integrity. Anything that would be inaudible in an audio path —
smoothing a parameter change, dithering, a DC-blocking filter, a host deciding a
buffer is "silent" and substituting zeros — is a wrong voltage here.

## Output convention

Samples are **normalized full-scale in `[-1, +1]`**. A plug-in never knows about
volts: full-scale voltage is a property of the interface, and differs between
devices and sometimes between outputs on one device. Each plug-in exposes
`Output Scale` and `Invert` as per-instance calibration (some interfaces wire
their outputs with reversed polarity; without `Invert` the suite is unusable on
them).

## Plug-ins

| Name | What it does |
|------|--------------|
| `DC` | Holds one constant value. The connection tester, and the suite's bit-exactness guard. |

Built for VST3, AU, and CLAP.

## What is and isn't verified

`test_dc.cpp` proves that `Processor::process()` holds a value bit-exactly
across block sizes and sample rates, and that a fresh instance emits zero. That
is a real guarantee, and it is not the whole chain.

Two links past it are **not** covered by these tests:

1. **A correct buffer can still be discarded by the host.** An adapter may hand
   the host the right samples flagged as silent, and the host will substitute
   zeros. That is a framework-level property, asserted at the adapter boundary
   in `test/test_au_v2_effect.cpp` and `test/test_vst3_plugin_state.cpp` (tagged
   `[silence]`), not here.

2. **A sample value becoming a real voltage needs hardware.** Nothing in this
   repo can measure a jack. Full-scale voltage, output polarity, DC coupling,
   and end-to-end latency are all unmeasured, and every claim that depends on
   them is open. The verification debt is tracked in the planning repo.

Do not describe these plug-ins as hardware-verified until that measurement
happens.

## Development

These are developed inside the Pulp tree against the live SDK, and are extracted
to their own public repo once the SDK changes they depend on ship in a release.

```bash
cmake --build build --target brew-dc-test && ./build/examples/bitches-brew/dc/brew-dc-test
cmake --build build --target BrewDC_VST3 BrewDC_CLAP BrewDC_AU
```

## Provenance

Written from documented, observable behavior of DC-coupled CV workflows and from
public modular-synth conventions (1V/oct, DIN sync, gate/trigger levels). No
third-party plug-in's code, scripts, art, or binaries were consulted or copied.
