# Bringing up the rig

Everything in this suite is proven by golden-vector tests and by nothing else. No
plug-in here has emitted a measured volt. This document is the procedure for
changing that, and the record of what has already been established.

The tool is `brew-rig`, built from `rig/`. It refuses to emit without `--armed`,
clamps its probe level to half full scale, and writes zeros on every exit path
including Ctrl-C.

## Before anything: make the rig safe

**Move the interface off "default output device."** A DC-coupled interface wired to
a modular is a machine where every macOS alert sound is a voltage on the CV bus.
`brew-rig` always opens a device by name, never the default, so nothing is lost.

```
System Settings > Sound > Output > (anything that is not the interface)
```

Then confirm what is patched. `brew-rig` will not emit without `--armed`, and you
should not arm it without knowing what is on the far end of every cable.

## What is already known

Established 2026-07-09 on an Apogee Symphony I/O ThunderBridge (32 in / 32 out,
48 kHz) feeding a DC-coupled ADAT-to-CV expander:

| Fact | Value |
|------|-------|
| Expander output 1 | host output channel **8** (a DAW shows this as **9**) |
| Expander outputs 1–8 | host output channels **8–15** (DAW 9–16) |
| The clock/trigger expander | rides on expander channel 7 (DAW output 15) — **never drive it with raw CV** |

That second expander wants an encoded bitstream its own vendor defines. This suite
emits raw CV and deliberately implements no such encoder, so raw DC on its channels
is noise it cannot decode. Pass `--skip 14,15` on any sweep.

Substitute your own interface's numbers. The only ones that transfer are the shape
of the problem: a DC-coupled expander appears as a block of ordinary output
channels at some offset, and at least one of them may not be CV at all.

Still unknown, and the reason this document exists:

- **How many volts is full scale?** Nothing in this suite may print a voltage until
  this is measured. It is also what the Quantizer's calibrated mode is blocked on —
  calibration maps a note to a *voltage*, and there is nothing to calibrate against
  without it.
- **Polarity.** Does a positive sample produce a positive voltage? The `Invert`
  control exists because at least one interface is known to wire its outputs
  backwards, and we cannot vouch for our own chain, let alone anyone else's.
- **Trigger width after the DAC.** A short pulse becomes a blip once the
  reconstruction filter has had it. The ~1 ms floor in `pulse.hpp` is a *guess*.

## Measuring full scale — the direct route

This is the shortest path and the one to take. It removes the interface's inputs,
the DAW, and any oscillator's tracking error from the chain.

1. Probe on **expander output 1**: tip to the probe, sleeve to ground.
2. Scope channel to **DC coupling** (not AC — an AC-coupled scope shows a DC level
   as zero, which is the same mistake an AC-coupled interface makes).
3. `Measure` → `Mean`.
4. Hold a known level and read the number:

```
brew-rig hold --device Symphony --armed --out 8 --level 0.25 --seconds 30
```

Full scale is `reading ÷ 0.25`. Around 2.5 V means a ±10 V interface; around
1.25 V means ±5 V.

Then repeat at `--level -0.25`. The reading must be the same magnitude with the
opposite sign. If the sign does not flip, the chain inverts and every plug-in on
this rig needs `Invert` on. If the magnitudes differ, the converter is doing
something to the negative half that no amount of software testing would surface.

## Measuring full scale — the ear route

Works, but inherits the oscillator's tracking error, and is only worth it if no
meter or scope is to hand.

Patch **expander output 1 → the oscillator's `1V/OCT` input** — that exact jack, not
an `FM` or `LIN FM` input, and turn up any attenuator beside it. Route the
oscillator's audio back to the DAW and put a tuner on it.

Read the pitch at rest, then under `--level 0.25`. One octave is one volt, by
definition of the input, so:

```
octaves    = log2(f_driven / f_rest)
volts      = octaves          # at 0.25 full scale
full scale = volts / 0.25
```

Pitch rising for a positive level also settles polarity.

**This was attempted on 2026-07-09 and failed.** Expander output 1's indicator lit,
so the voltage reached the jack, but the oscillator's pitch never moved. Untriaged;
the suspects are a wrong jack, an attenuator at zero, or a dead monitoring path.
Use the scope.

## The automatic route (needs a loopback)

With the interface's outputs wired through the modular and back into its inputs —
e.g. a CV output back into a DC-coupled input — the crossbar discovers itself:

```
brew-rig listen --device Symphony --seconds 3          # emits nothing; which input is live?
brew-rig map    --device Symphony --armed --from 8 --to 13 --skip 14,15
```

`map` drives each output channel in turn and reports which input responded, and
with what signed gain. A gain near `+1` is a straight wire; near `-1` the chain
inverts.

**A loopback can never measure volts.** It is dimensionless: full-scale out
arriving as full-scale in proves the chain is unity gain and says nothing about
what was on the wire. Use `hold` and an instrument.

## Traps, each of which has already cost an hour

**Do not open the interface at its first advertised sample rate.** The Symphony
advertises `44100` first and *runs* at `48000`. Opening it at 44.1 k reconfigures
its clock, which drops the ADAT link to the expander — and that looks exactly like
a bad cable. `brew-rig` pins `--rate` (default 48 kHz, also the only rate at which
all eight ADAT channels exist) and refuses a rate the device does not advertise.

**An SSH session on macOS cannot record.** TCC denies microphone access and the
denial is *silent*: the device opens, delivers the expected frame count, and every
sample is exactly `0.0f`. A real converter input always has a noise floor, so
`brew-rig` reports exact zeros across every channel as a permissions problem rather
than as an unpatched cable. Grant `/usr/libexec/sshd-keygen-wrapper` Microphone
access in System Settings, or run from a GUI session. Output is not gated, which
is the uncomfortable asymmetry: a remote shell can drive a modular it cannot hear.

**Typing while a command runs cancels it.** Every `brew-rig` command that emits
prints a three-second countdown and a completion line. If you did not see
`done — outputs are at zero`, it did not run, and whatever you saw on the rack came
from something else.

**A lit indicator is not a voltage.** It says a signal arrived at the jack. It says
nothing about magnitude, and nothing about what happened after the jack.
