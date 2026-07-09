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
smoothing a parameter change behind the user's back, dithering, a DC-blocking
filter, a host deciding a buffer is "silent" and substituting zeros — is a wrong
voltage here.

The rule is *nothing smooths unless you asked*, not *nothing smooths*. An
explicit slew control is a portamento, and a modular has slew limiters in it for
exactly that reason. DC has one, off by default; at zero it is a wire, and the
bit-exactness tests still hold.

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
| `DC` | Holds one constant value, optionally shaped by the input and slewed. The connection tester, and the suite's bit-exactness guard. |
| `Sync` | A clock pulse train and a run/stop gate, locked to the host transport. |
| `LFO` | A modulation source locked to the tempo or free-running in hertz, plus the same shape a quarter cycle ahead. |
| `Function` | Math on an incoming control voltage: a curve, plus scale and offset at each end. |
| `Quantizer` | Snaps an incoming control voltage to discrete steps. |
| `Step LFO` | An eight-step pattern and a gate, locked to the host. |
| `CV To OSC` | Passes a control voltage through and reports it over OSC. Off by default. |

`Function` is the only plug-in here that reads its input bus — the others
generate. It offers five curves, and they are not all the same kind of thing.

`Linear`, `Exponential` (`2^x - 1`), `Logarithm` (`1 + log2(x)`, zero for
non-positive input) and `Absolute` are the conventional definitions, and they are
the defaults, because a patch written against any other CV utility expects them.
They are also poorly behaved on a bipolar signal, and honestly so: `2^x - 1` is
not odd-symmetric, so it shifts the centre of a symmetric LFO; and the logarithm
is undefined below zero, so it flattens half the range and dives toward negative
infinity near the origin, where the output clamp catches it. Those are properties
of the functions, not of this implementation.

`Power` is ours: `y = sign(x) · |x|^k`, the obvious single-parameter family for a
bipolar signal. Odd-symmetric, so polarity survives. Monotone, so it never folds.
Fixed at the origin and both rails for every `k`, so `Amount` bends the middle of
the response without ever moving where full scale lands. And `k` and `1/k` are
exact inverses, so one knob spans both directions. Reach for it when the signal
swings both ways; reach for the conventional pair when you are reproducing
someone else's patch. `Amount` drives `Power` and nothing else — the editor shows
a dash on the other curves rather than a stale number.

Two consequences worth naming. Its defaults are a **bit-exact wire**, so a
freshly inserted Function can never be the thing that changed a voltage. And its
bypass is a **wire, not a mute** — the opposite of the generators, because muting
an insert would drop the voltage the plug-in *upstream* of it is generating.

`LFO` is a **mixer, not a selector**. Sine, triangle, saw and square each have
their own bipolar depth and are summed, so a shape can be subtracted as easily as
added and everything between the four is reachable. One depth at full and the rest
at zero is the single-shape behaviour you would expect. `Asymmetry` moves the
waveform's centre in time — a pulse-width control generalized to every shape — and
`Random` is a sample-and-hold, one level per cycle held flat across it.

The sum is not clamped inside the mixer. Four depths at full reach 4.0, and
flattening that before `Offset` and the output scale have had their say would
silently discard a mix you asked for. It clamps once, at the jack.

`Smooth` is the one control here that carries state, and it is off by default for
that reason. At zero it is a wire, bit for bit. Turned up it slews (positive) or
low-passes (negative) the shape before the output stage, exactly as `DC`'s does —
which means a bounce from a fixed start is still identical every render, but a
locate into the middle of a project agrees with a playthrough only after the
smoother has settled. That transient is bounded by the time constant you dialled
in, and it is the honest price of a slew limiter on a generator.

`Swing` warps the beat timeline the same way `Sync`'s does, and for the same
reason it is applied to the *position* before the position becomes a phase: warp
the phase afterwards and the LFO stops agreeing with the clock it is supposed to
shuffle alongside. 50% is straight, bit-identically so. It has no meaning in free
run — a hertz rate that shuffled would just be a wrong hertz rate — so it is
ignored there rather than approximated.

`Free Run` swaps the tempo for `Free`, a rate in hertz. Both are derived from the
host's position — one from `position_beats`, one from `position_samples` — so free
running is not free *floating*: it stays a pure function of where the playhead is,
and keeps every property below. It is what a modulation that should ignore a tempo
map needs, and the two rate knobs stay visible together so switching modes never
moves a control out from under the mouse.

Its phase is derived from the host's position rather than accumulated per block,
so a bounce lands the modulation on the same samples every time, a locate puts the
LFO where the timeline says it should be, and a long session cannot drift against
the host. The `Random` level is a **hash of the cycle index**, not a generator, for
exactly that reason: render twice, get the same samples. Reroll it with `Seed`, not
by pressing play again. The second output leads the first by a quarter cycle: fed
into two CV inputs the pair traces a circle, which is how one oscillator drives a
two-axis modulation — and its sample-and-hold is keyed on the cycle *its own* phase
sits in, or the circle would tear at every cycle boundary. Rendered per sample: a
block-rate control voltage is an audible zipper on whatever it drives.

`Sync` also carries a **1st Delay** (hold the clock off for N ms after the
transport starts, measured from the run origin so two runs behave identically)
and a bipolar **Offset**. The offset exists because a DAC, its reconstruction
filter, and the receiving gate input all add latency: a clock that is
sample-accurate in software arrives late at the hardware, and a negative offset
pulls the pulses back ahead of the beat.

Swing and periodic reset are named in the suite's feature list but are **not
implemented**. Their behavior is not specified anywhere this project is permitted
to derive it from, and a plausible guess would ship as a green test asserting the
guess. Same for FSK tape sync, which additionally needs a continuous phase
accumulator designed rather than bolted on.

Built for VST3, AU (`aufx`), and CLAP. `brew-core/` holds what they share: the
output stage above, the clock grid, the pulse-width rules, and the run-segment
origin. `brew-ui/` holds the shared editor furniture.

## The editors exist because CV is invisible

A CV plug-in makes no sound and drives no meter, so from inside a DAW there is no
way to tell "holding +0.5 full scale" from "cable unplugged". Every plug-in
therefore shows what is actually leaving the jack: `DC` draws a bipolar rail with
a marker at its current output, `Sync` lights a CLOCK and a RUN lamp, `LFO` traces
its own output, and `Function` plots its curve with a dot at the point the signal
is currently sitting on it. The scope and the graph call the same `value_at()` and
`function_transfer()` the DSP does, so the picture cannot drift from the signal.
All four read real DSP state, published once per block from the audio thread.

The rail reads in **normalized full scale, never volts**. The plug-in cannot know
the interface's rail voltage, and printing a number it cannot know is a lie the
user would wire into a modular.

Controls are Pulp's own widgets, laid out with flex. `test_brew_ui.cpp` renders
each editor headless and asserts the readouts *move with the state* — a
screenshot test that only checks "the PNG is non-empty" passes on a blank canvas,
and on a plug-in this quiet a dead readout is indistinguishable from a dead cable.

Each plug-in overrides `editor_size()`, and both the test and the screenshot
dumper render at whatever it returns. Without the override a host opens the editor
at `Processor`'s 400×300 default, and a screenshot taken at any other geometry is
a picture of a layout no DAW will ever show.

## Nothing here has emitted a measured volt

The suite's correctness is covered by golden-vector tests. Its *hardware* claims
are covered by nothing at all, and this README will not pretend otherwise. A
sample value becoming a voltage, the polarity of that voltage, which host channel
arrives at which jack, whether a 1 ms trigger survives the DAC's reconstruction
filter — none of that is knowable from a datasheet, and none of it is verified.

`brew-rig` is the tool that closes what can be closed. Wire the interface's
outputs through the modular's CV inputs and back into its inputs, and it will
sweep one output channel at a time to discover the crossbar and the polarity of
the chain. It emits nothing without `--armed`, clamps its probe level to half
full scale, and writes zeros on every exit path including Ctrl-C — a CV tool that
leaves a voltage on a jack when it dies is worse than no tool.

What a loopback can never tell you is **volts**. A closed loop is dimensionless:
full-scale out arriving as full-scale in proves the chain is unity gain and
proves nothing about what was on the wire. `brew-rig hold` parks a DC level on
one channel so a meter or a scope can answer that, and until it is answered
nothing in this suite may print a voltage.

[HARDWARE.md](HARDWARE.md) is the bring-up procedure, the channel map established
so far, and the traps — each of which has already cost an hour.

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
