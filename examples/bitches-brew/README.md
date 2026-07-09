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
| `Sync` | A clock pulse train and a run/stop gate, locked to the host transport. |

Built for VST3, AU (`aufx`), and CLAP. `brew-core/` holds what they share: the
output stage above, the clock grid, the pulse-width rules, and the run-segment
origin.

## The clock is derived, never accumulated

`Sync` computes which clock edges fall inside a block from the host's reported
position. It does not advance a phase counter one block at a time.

This is the single most important decision in the suite. An accumulator has to
*catch up* whenever the host moves the playhead — and the audible result is a
burst of pulses at the instant the transport starts, arriving downstream as a
fistful of spurious clock ticks. Deriving from position means beat 3.7 maps to the
same edge regardless of how the playhead got there, so there is nothing to catch
up. `test_sync.cpp` asserts the exact multiset of edge offsets for ten transport
scenarios — play from the top, play from mid-timeline, stop/play cycles, loop
wrap, forward locate, tempo change, bar wait, a host re-rendering the same block,
a host lying about `transport_jump`, and no transport at all.

The features that genuinely *are* run-relative (skip the first pulse, wait for the
bar) hang off one explicit origin captured on the play edge. That is the whole of
the plug-in's musical state.

Two physical constraints bound the pulse width, and `Sync` clamps to both. A
one-sample pulse does not survive a DAC's reconstruction filter, so there is a
~1 ms floor. And a pulse as long as the clock period never falls — a welded gate —
so the width is held strictly under half the period. At 24 ppqn and 300 BPM the
period is 8.3 ms, which a perfectly reasonable-looking 10 ms trigger length would
weld. The ceiling wins when the two conflict: a weak trigger beats a stuck one.

## What is and isn't verified

`test_dc.cpp` proves that `Processor::process()` holds a value bit-exactly across
block sizes and sample rates, and that a fresh instance emits zero. `test_sync.cpp`
proves the clock's response to every transport scenario above. Those are real
guarantees, and they are not the whole chain.

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
cmake --build build --target brew-core-test brew-dc-test brew-sync-test
./build/examples/bitches-brew/core/brew-core-test
./build/examples/bitches-brew/dc/brew-dc-test
./build/examples/bitches-brew/sync/brew-sync-test

cmake --build build --target BrewDC_VST3 BrewDC_CLAP BrewDC_AU
cmake --build build --target BrewSync_VST3 BrewSync_CLAP BrewSync_AU
```

## Provenance

Written from documented, observable behavior of DC-coupled CV workflows and from
public modular-synth conventions (1V/oct, DIN sync, gate/trigger levels). No
third-party plug-in's code, scripts, art, or binaries were consulted or copied.
