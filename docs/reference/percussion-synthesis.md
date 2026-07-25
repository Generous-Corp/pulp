# Percussion synthesis

Pulp synthesises drums rather than playing samples of them. This page covers
what the voices are, how they are put together, and where the models come
from.

The classes live in `pulp::signal::drum` (`core/signal/include/pulp/signal/drum/`)
and are ordinary DSP building blocks: a plugin composes them inside its
`Processor::process()`. A drum kit is not a `SignalGraph` — its topology is
fixed at build time, so it belongs in a `Processor`. See
[processing-models.md](processing-models.md).

## Why synthesis

A sampled drum is one recording of one hit. Every trigger reproduces it
exactly, so a fast pattern reads as a machine gun, and velocity can only
change the level or cross-fade between a handful of captured layers. A
synthesised drum has no such ceiling: a hard hit genuinely bends further and
opens up, a second strike during the tail interferes with what is still
ringing, and the whole instrument tunes.

The cost is that each voice has to be built from what the drum actually does,
which is why the voices below are not variations of one generic
oscillator-plus-envelope.

## The shared layer

Every voice is assembled from the same components, all in `pulp::signal`:

| Component | What it is for |
|---|---|
| `NoiseSourceT` | Deterministic colourable noise. Reseeded per hit, so a render is reproducible. |
| `DecayEnvelopeT` | One-shot attack / hold / decay, in either RC or T60 units. |
| `TwoPoleResonatorT` | A single struck resonance. `ModalBankT` is the many-mode case. |
| `LofiChainT` | Bit quantisation, sample-and-hold rate reduction, dead-zone saturation. |
| `LowpassGateT` | Vactrol dynamics: fast on, slow off, quiet implies dark. |
| `BridgedTResonator` | The TR-808 bass-drum network, modelled at component level. |
| `SquareOscBankT` | An inharmonic oscillator cluster — the metallic source. |

And from `pulp::signal::drum`:

| Component | What it is for |
|---|---|
| `Voice` | The lifecycle: additive render, faded choke, velocity reaching timbre. |
| `VelocityResponse` | How hard a hit changes the sound, beyond changing its level. |
| `OutputStage` | Fold → saturate → degrade → level, in that fixed order. |
| `ClickLayer` / `NoiseLayer` / `SubLayer` | The transient, the air, the octave-down reinforcement. |
| `Kit` | Sums voices and runs choke groups. |

## The voices

| Voice | What distinguishes it |
|---|---|
| `KickVoice` | Three bodies: a swept oscillator, a struck resonance, or the modelled TR-808 circuit whose pitch drop emerges from its own ring rather than from an envelope. |
| `SnareVoice` | Two instruments in one shell — a beating tone pair for the drum, filtered noise for the wires — plus the wires' own buzz and a second lo-fi chain for the noise path alone. |
| `HatVoice` | One voice for closed hat, open hat, ride and crash; the decay control *is* that continuum. Source is an inharmonic square-oscillator cluster. |
| `ClapVoice` | A scheduled burst train with a tail underneath that fuses it into one event. |
| `TomVoice` | Decoupled pitch and amplitude envelopes — the dive lands while the note rings on — and velocity deepens the dive. |

## Velocity is not a volume control

`VelocityResponse` is the contract that separates these voices from a sample
player triggered at different gains. A drum struck harder is not the same
sound turned up: the head deflects further so the pitch bends further, the
stick excites more high partials, and the balance shifts toward the transient.

Every voice consumes the response rather than reading velocity directly, and
the test suites assert it by level-normalising a soft and a hard hit and
requiring a spectral difference to remain — with a negative control that
zeroes the timbral terms and requires the two renders to differ by exactly a
scalar.

## Realtime contract

`prepare()` may allocate. Everything else — `note_on`, `choke`, `process`,
`reset` — allocates nothing and takes no locks, and each suite holds that with
an allocation probe.

The lo-fi chain and the output stage distort, so they generate harmonics above
the input's bandwidth and will alias at the host rate. Callers that care run
them inside an oversampled section (`pulp::signal::Oversampling`); callers
modelling hardware that itself aliased run them at the host rate deliberately.

## Provenance

Every voice here is an original implementation written from published models
and public documentation. No drum-machine firmware, sample library, or
third-party synthesis source is vendored, transcribed, or matched against.

Where a model comes from a paper, the class comment cites it and the
implementation follows the paper rather than any existing code of it:

- Werner, Abel & Smith, "A Physically-Informed, Circuit-Bendable, Digital
  Model of the Roland TR-808 Bass Drum Circuit", DAFx-14 — the bridged-T
  network and its transistor leakage fit, used by `KickBody::circuit`.
- Parker & D'Angelo, "A Digital Model of the Buchla Lowpass Gate", DAFx-13 —
  the vactrol response family in `LowpassGateT`.
- Zavalishin, *The Art of VA Filter Design*, and Simper's state-variable
  filter note — the TPT forms used throughout `pulp::signal`.
- Fletcher & Rossing, *The Physics of Musical Instruments* — circular-membrane
  mode ratios, for modal bodies.
- Roland TR-808 service documentation — the hi-hat oscillator bank's
  frequencies, stored here as ratios.

Two deliberate exclusions:

- **No parameter sets derived from commercial libraries.** Preset values are
  chosen from the published models and from what each control is musically
  useful over, never fitted against a copyrighted sample library.
- **No vendored synthesis engines.** Where a well-known open-source
  implementation of the same published technique exists, Pulp implements the
  technique from its source paper instead, so there is no license to inherit.
