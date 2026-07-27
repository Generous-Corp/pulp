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
| `MembraneVoice` | A circular-head modal bank with strike position, mode structure, stretch, damping, and selectable noise-burst or impulse excitation. |
| `CymbalVoice` | A deterministic comb plate whose mode spacing, frequency shift, high-mode decay, strike, and per-hit variation are independent. |
| `StringVoice` | Karplus–Strong string with physical damping/stiffness/pluck controls and optional FM, ring, or sync post-body modulation. |
| `ZapVoice` | Casio-style phase-distortion percussion with independent pitch and distortion envelopes. |
| `FmDrumVoice` | Two-operator FM with independent wave, warp, LFO, transient, noise, click, and output-filter controls. |
| `Fm6DrumVoice` / `Fm8DrumVoice` | Six- and eight-operator matrices with per-operator ratios, levels, and decays; the eight-operator voice also exposes per-operator feedback and waves. |

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
the input's bandwidth. Every voice exposes `set_output_oversampling()` with
`bypass`, `x2`, or `x4`; `latency_samples()` reports the resulting fixed
host-rate delay. Pick that quality during setup because changing it resets the
output stage and changes latency.

## Cookbook

### Render one hit

```cpp
#include <pulp/signal/drum/kick.hpp>

pulp::signal::drum::KickVoice kick;
kick.set_tune_hz(52.0);
kick.set_body_decay_ms(650.0);
kick.set_output_oversampling(
    pulp::signal::drum::OutputOversampling::x2);
kick.prepare(sample_rate);       // setup thread
kick.note_on(0.85f);             // audio thread
kick.process(output, frames);    // adds; clear output first when appropriate
```

`process()` is additive. This is what lets several voices render into one kit
buffer without another mixing allocation.

### Choke an open hat

```cpp
open_hat.note_on(velocity);
// Later, when a closed hat arrives:
open_hat.choke(4.0f);
closed_hat.note_on(velocity);
```

`choke()` fades instead of clearing state abruptly, so the cut does not create
a broadband click.

### Construct from a persistent engine name

```cpp
using namespace pulp::signal::drum;
if (const auto* metadata = find_engine(saved_name);
    metadata != nullptr && metadata->available) {
    std::unique_ptr<Voice> voice = create_engine(metadata->id);
    voice->prepare(sample_rate);
}
```

Unavailable registry entries remain discoverable so a loader can distinguish a
known, license-held engine from an unknown identifier.

### Bake a Forge source node

```cpp
#include <pulp/host/forge_drum_catalog.hpp>

using namespace pulp::host::forge_drum;
auto type = make_drum_node(
    pulp::signal::drum::EngineId::membrane_modal);
graph.register_custom_node_type(type);
```

The node has no audio inputs and two outputs. Inject `kVelocity`, then raise
`kTrigger` through 0.5 to start a hit. Lower `kTrigger` before the next hit.
`kChoke` is another rising-edge control and `kChokeMs` sets its fade.

## Complete public API

All ranges below are plain-domain values. Setters clamp unless noted. Defaults
are those of a newly constructed voice before a preset or caller changes it.
Every concrete voice also inherits the `Voice` methods in the first table.

### `Voice`

| Method | Contract |
|---|---|
| `prepare(sample_rate)` | Fix rate, prepare derived storage, then reset. Non-positive rates retain the current rate. Setup-time; derived voices may allocate. |
| `reset()` | Immediate silence and complete DSP-state reset. |
| `note_on(velocity)` | Start/retrigger; velocity clamps to 0–1. |
| `choke(fade_ms = 4)` | Linear fade to silence; minimum one sample. |
| `is_active()` / `state()` | Tail/lifecycle inspection (`idle`, `ringing`, `choking`). |
| `velocity()` / `sample_rate()` | Current clamped velocity and prepared rate. |
| `latency_samples()` | Fixed delay introduced by selected output quality. |
| `output_oversampling()` / `set_output_oversampling(factor)` | Inspect/select `bypass`, `x2`, or `x4`; select during setup because it resets state and changes latency. |
| `set_velocity_response(response)` / `velocity_response()` | Replace/inspect the per-voice level, bend, brightness, and noise-balance velocity law. |
| `process(out, count)` | Add mono output; null, non-positive, or inactive calls are no-ops. |
| `process_stereo(left, right, count)` | Add stereo output; mono-only voices are centred with exact mono-sum equivalence. |

### Shared `OutputStage`

`output()` returns this stage on every concrete voice.

| Method | Range/default and meaning |
|---|---|
| `prepare(rate)` / `reset()` / `reset_nonlinear_state()` | Setup, full reset, or lo-fi-only reset. |
| `set_oversampling(factor)` / `oversampling()` | `x2` default, `bypass`, or `x4`; resets and changes latency. |
| `latency_samples_for(factor)` / `latency_samples()` / `has_tail()` | Static latency query, selected latency, and pending linear/lo-fi tail. |
| `set_drive(amount)` | 0–1, default 0; tanh pre-gain. |
| `set_fold(amount)` | 0–1, default 0. |
| `set_level(linear)` | 0 or greater, default 1. |
| `set_ahd_ms(attack, hold, decay)` | Non-negative ms (decay floor 0.01); configures and enables the post-saturation VCA. Defaults 0/0/1000. |
| `set_ahd_enabled(enabled)` / `ahd_enabled()` / `ahd_gain()` | Enable/inspect the AHD and its current gain. |
| `sync_configuration_from(other)` | Copy controls without copying FIR, RNG, lo-fi, tail, or envelope history. |
| `trigger()` / `process(sample)` | Start the AHD and process fold → drive → lo-fi → level/AHD. |
| `lofi()` | Access bit depth (1–24, default 24), hold rate (1 Hz or greater, default transparent), jitter/smoothing (0–1), and dead zone (0–0.9). |

### Voice-specific setters

The table is exhaustive for musically meaningful public setters. Inspection
accessors (`body()`, `hit_life()`, `mode_gain_energy()`, `algorithm()`, and
`output()`) return the corresponding current selection or measurement.

| Voice | Methods — range; default |
|---|---|
| `KickVoice` | `set_body(KickBody)` — construction topology, oscillator; `set_tune_hz` 20–400 Hz, 55; `set_body_decay_ms` 10–4000, 400; `set_pitch_sweep_octaves` 0–6, 2 and `set_pitch_sweep_ms` 0.5–500, 30 (oscillator body only — the resonant and circuit bodies ignore both, so the Forge catalog does not declare them there); `set_click_level` 0+, 0.3; `set_click_tone_hz` 20–20000, 4000; `set_click_decay_ms` 0.05–200, 2; `set_noise_level` 0+, 0; `set_noise_decay_ms` 0.5–4000, 60; `set_noise_color` five `NoiseColor` values, white; `set_sub_level` 0+, 0; `set_oscillator_triangle` bool, false; `set_fm_amount` 0–8, 0; `set_fm_ratio` 0.25–16, 1; `set_circuit_components` validated physical component values; `set_circuit_feedback` 0–1, 0.85; `set_circuit_attack_ms` 0–50, 4; `set_circuit_pulse_ms` 0.2–20, 2; `set_circuit_drive` 0–1, 0.3; `set_circuit_sigh` bool, true; `set_circuit_hit_life` a `HitLifeMode`, `preserved_state` for registry circuit construction. |
| `SnareVoice` | `set_tune_hz` 60–800, 180; `set_tone_ratio` 1–4, 1.6; `set_tone_level` 0+, 0.5; `set_tone_decay_ms` 5–2000, 120; `set_pitch_sweep_octaves` 0–4, 0.5; `set_pitch_sweep_ms` 0.5–300, 25; `set_fm_amount` 0–8, 0; `set_ring` 0–1, 0; `set_noise_level` 0+, 0.6; `set_noise_decay_ms` 5–3000, 180; `set_noise_color` five values, white; `set_noise_cutoff_hz` 100–18000, 3000; `set_noise_resonance` 0.5–12, 1; `set_noise_sweep_octaves` -4–4, 0; `set_rattle` 0–1, 0; `set_rattle_hz` 5–400, 45; `set_snap_level` 0+, 0.4; `set_snap_cutoff_hz` 20–20000, 6000; `set_snap_decay_ms` 0.05–200, 4; `set_shell_level` 0+, 0; `set_shell_resonance` 1–30, 12; `tone_lofi()` and `noise_lofi()` expose independent `LofiChain`s. |
| `HatVoice` | `set_tune_hz` 40–4000, 320; `set_decay_ms` 5–8000, 60; `set_spread`, `set_metal`, `set_grit` 0–1, defaults 1/0.8/0; `set_grit_ratio` 0.25–16, 1.4; `set_cutoff_hz` 200–18000, 7000; `set_resonance` 0.5–12, 1.2; `set_bandpass` bool, false; `set_noise_color` five values, white. |
| `ClapVoice` | `set_burst_count` 1–8, 4; `set_burst_spacing_ms` 1–60, 11; `set_burst_decay_ms` 0.5–60, 6; `set_burst_falloff` 0.2–1.5, 0.82; `set_gap_jitter` 0–1, 0.35; `set_alternate_polarity` bool, false; `set_stereo_width` 0–1, 1; `set_tail_level` 0+, 0.35; `set_tail_decay_ms` 10–3000, 180; `set_cutoff_hz` 200–16000, 1400; `set_resonance` 0.5–12, 1.4; `set_noise_color` five values, white; `set_body_level` 0+, 0; `set_body_hz` 40–2000, 300. |
| `TomVoice` | `set_tune_hz` 30–1200, 120; `set_bend_octaves` 0–6, 1; `set_bend_ms` 0.5–500, 30; `set_decay_ms` 10–4000, 500; `set_wave` triangle/sine, triangle; `set_noise_balance` 0–1, 0.15; `set_noise_cutoff_hz` 50–16000, 1500; `set_noise_resonance` 0–1, 0.3; `set_noise_color` five values, white; `set_click_level` 0+, 0; `set_click_cutoff_hz` 20–20000, 4000; `set_click_decay_ms` 0.05–200, 2; `body_lofi()` and `noise_lofi()` expose independent `LofiChain`s; `apply_preset` applies one stable preset during setup. |
| `MembraneVoice` | `set_tune_hz` 20–2000, 90; `set_structure`, `set_stretch`, `set_damping`, `set_brightness`, `set_spread` 0–1, defaults 1/0/0.5/0.5/0.1; `set_decay_ms` 20–8000, 700; `set_position` 0.02–0.5, 0.28; `set_exciter_ms` 0.1–50, 1.5; `set_exciter_cutoff_hz` 100–18000, 6000; `set_exciter` noise-burst/pluck, noise-burst; `set_sub_level`, `set_air_level`, `set_click_level` 0+, 0; `set_air_decay_ms` 1–500, 20; `set_click_decay_ms` 0.1–50, 2; `gate()` exposes the shared low-pass-gate controls. |
| `CymbalVoice` | `set_tune_hz` 40–2000, 320; `set_decay_ms` 50–8000, 1800; `set_decay_tilt` 0.5–1, 0.93; `set_high_mode_emphasis_db` -18–18, 0; `set_velocity_feedback` 0–1, 0.35; `set_velocity_high_mode_db` 0–18, 4; `set_upper_highpass_hz` 0–8000, 500; `set_spread`, `set_inharmonicity` 0–1, 1/0.4; `set_shift_hz` -400–400, 45; `set_noise_level`, `set_strike_level` 0+, 0.7/0.3; `set_strike_ms` 0.5–200, 6; `set_tone_hz` 500–20000, 12000; `set_low_cut_hz` 20–2000, 300; `set_variation` 0–1, 0.5, selecting one of the two body-preserving policies; `set_hit_life` five lifecycle policies; `set_noise_color` five values, white. |
| `StringVoice` | `set_tune_hz` 30–4000, 220; `set_decay_seconds` 0.05–20, 2; `set_damping`, `set_stiffness` 0–1, 0.3/0; `set_pluck_position` 0–0.5, 0.25; `set_exciter_ms` 0.1–50, 1; `set_brightness_hz` 200–18000, 1800; `set_pick_direction` 0–0.99, 0; `set_restart_on_hit` bool, false; `set_modulation` none/FM/ring/sync, none; `set_modulation_mix` 0–1, 0; `set_modulation_ratio` 0.125–16, 2; `set_fm_depth_octaves` 0–2, 0.25; `set_lpg_amount` 0–1, 0; `gate()` exposes low-pass-gate controls; `set_noise_color` five values, white. |
| `ZapVoice` | `set_tune_hz` 20–4000, 220; `set_pitch_sweep_octaves` 0–6, 2.5; `set_pitch_sweep_ms` 0.5–500, 35; `set_decay_ms` 10–4000, 350; `set_shape` five `PhaseDistortionShape` values, resonant-saw; `set_distortion` 0–1, 0.8; `set_distortion_ms` 0.5–500, 60; `set_resonant_depth` 1–32, 12; `set_detune_cents` 0–100, 9; `set_ring` 0–1, 0; `set_ring_ratio` 0.25–16, 1.5; `gate()` exposes low-pass-gate controls. |
| `FmDrumVoice` | `set_tune_hz` 20–4000, 110; `set_ratio` 0.1–24, 1.41; `set_index` 0–24, 6; `set_index_ms` 0.5–2000, 40; `set_decay_ms` 10–4000, 400; `set_pitch_sweep_octaves` 0–6, 0; `set_pitch_sweep_ms` 0.5–500, 30; `set_feedback` 0–1, 0; carrier/modulator `set_*_wave` 0–25, 0; `set_*_warp` 0–1, 0; `set_*_warp_ms` 0.5–2000, 40; `set_lfo_rate_hz` 0.05–40, 5; `set_lfo_depth_octaves` 0–2, 0; `set_lfo_delay_ms`, `set_lfo_fade_ms` 0–4000, 0; `set_hard_sync` bool, false; `set_transient` -1–last recipe, -1; `set_noise_level` 0+, 0; `set_noise_decay_ms` 0.5–500, 10; `set_noise_color` five values, white; `set_cutoff_hz` 40–18000, 12000; `set_resonance` 0.5–12, 0.8; `set_bandpass` bool, false; `set_click_level` 0+, 0.2; `set_click_cutoff_hz` 20–20000, 6000. |
| `Fm6DrumVoice` | `set_algorithm` 0–31, 4; `set_tune_hz` 20–4000, 110; per operator `set_operator_ratio` 0.1–32, `set_operator_level` 0–1, `set_operator_decay_ms` 1–4000; `set_feedback` 0–1, 0; `set_depth` 0–12, 3; `set_pitch_sweep_octaves` -6–6, 0; `set_pitch_sweep_ms` 0.5–1000, 40; `set_formant_hz` 40–18000, 3000; `set_formant_q` 0.5–12, 0.9. Invalid operator indices are ignored. |
| `Fm8DrumVoice` | `set_algorithm` 0–15, 9; `set_tune_hz` 20–4000, 110; per operator `set_operator_ratio` 0.1–32, `set_operator_level` 0–1, `set_operator_decay_ms` 1–4000, `set_operator_feedback` 0–1, and `set_operator_wave` 0–25; `set_depth` 0–12, 3; `set_formant_hz` 40–18000, 2000; `set_formant_q` 0.5–12, 1; `set_transient` -1–last recipe, -1; `set_noise_level` 0+, 0; `set_noise_decay_ms` 0.5–500, 10; `set_noise_color` five values, white; `set_click_level` 0+, 0. Invalid operator indices are ignored. |

### Registry and kit

| API | Contract |
|---|---|
| `engine_registry` | Stable metadata for all engine identities, including known-but-held `dx7.msfa`. |
| `find_engine(name)` | Metadata pointer or null. Names are persistence-safe. |
| `create_engine(id)` | Setup-time allocation of an available configured voice; null for held/unknown identities. |
| `Kit::add_voice(voice, note, choke_group = 0)` / `voice_count()` | Register a non-owned voice during setup and inspect the slot count. The returned slot index avoids an RT note lookup. |
| `Kit::prepare(rate)` / `reset()` / `is_active()` | Prepare all voices, clear all voices, or inspect aggregate activity. |
| `Kit::set_output_oversampling(factor)` / `output_oversampling()` / `latency_samples()` | Keep the summed kit on one aligned output-quality and latency contract. |
| `Kit::set_choke_fade_ms(ms)` | Set the group fade, floor 0.1 ms; default 4 ms. |
| `Kit::trigger(note, velocity)` / `trigger_slot(index, velocity)` | Choke peers, then trigger by note lookup or stable slot index. Unknown notes/slots are ignored. |
| `Kit::choke_group(group)` | Fade every voice in a nonzero group without an accompanying hit. |
| `Kit::process(out, count)` | Add every active voice to a mono buffer without allocation. |

## Forge catalog contract

`<pulp/host/forge_drum_catalog.hpp>` defines one stable type ID for each
available registry topology (`drum.kick.oscillator` through `drum.fm8`).
`dx7.msfa` deliberately has no node because its implementation remains on
license hold.

Every node is `lowerable`, has two output ports, declares honest plain-domain
`baked_params`, and consumes them sample-accurately through `BakedParamView`.
The common stable IDs are `kTrigger` (1), `kVelocity` (2), `kChoke` (3),
`kChokeMs` (4), velocity-response controls 5–8, tuning/decay/sweep IDs
10–14, and output-stage IDs 20–31.
Voice-specific semantic constants such as `kSnareRattle`,
`kMembraneStructure`, and `kOperatorRatioBase + operator` name the stable
node-local IDs in the header.

Construction-time choices are not injectable: engine/body identity determines
topology. Forge nodes force output oversampling to `bypass`, because the custom
node contract has no latency-reporting surface and an x2/x4 source would
misalign parallel graph paths. Runtime wave/algorithm selections are bounded
discrete baked controls because their setters are allocation-free and preserve
the node's storage topology. The cymbal exposes the explicit `kCymbalHitLife`
policy; `set_variation()` is a source-API convenience that selects one of those
same policies, not an independent baked parameter. `HitLifeMode` names each
combination of excitation and body behaviour explicitly, so a voice never
encodes a policy outside the enum. Non-finite injected
values are sanitized before reaching DSP state.

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
