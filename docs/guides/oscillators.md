# Oscillator suite

`pulp::signal::osc` (`core/signal/include/pulp/signal/osc/`) is a family of
seven headers that compose into four oscillator "flavors" — VA, VCO, DCO, and
WT (in two tiers) — over two shared primitives, a phase accumulator and a set
of BLEP/BLAMP anti-aliasing kernels. This is a **different, newer class
family** than `signal::Oscillator` (`oscillator.hpp`, documented in
[Signal Processing §3](signal-processing.md#3-oscillators)) — that one is a
simpler polyBLEP oscillator with float phase and an integrated triangle;
extend the suite on this page for new work, and see `oscillator.hpp`'s own
header comment for exactly how the two differ.

Each family is a standalone class — pick the one whose character matches
what you're building, not a shared base class. All of them take a per-sample
phase increment (`frequency / sample_rate`) rather than holding a frequency
internally, so through-zero FM and per-sample pitch modulation compose
without any API change.

```cpp
#include <pulp/signal/osc/va.hpp>

pulp::signal::osc::VaOscillator osc;
osc.set_shape(pulp::signal::osc::VaShape::saw);

for (int i = 0; i < num_samples; ++i)
    output[i] = static_cast<float>(osc.next(440.0 / sample_rate));
```

## Which one do I want?

| Family | Reach for it when you want... | Header |
|---|---|---|
| VA | The default bandlimited analog shapes — clean, deterministic, hard sync, through-zero FM | `va.hpp` |
| VCO | The above plus analog "imperfection": drift, jitter, waveshaping, a bowed ramp | `vco.hpp` |
| DCO | Crystal-stable pitch with the *quantization* character of a divider-clocked synth, not drift | `dco.hpp` |
| WT (modern) | A wavetable set with smooth scan/morph and click-free band-switching | `wt.hpp` |
| WT (lo-fi) | Deliberately gritty ZOH wavetable playback — pitch-tracking aliasing as the point | `wt_lofi.hpp` |

DCO and VCO both build on VA's shape stage (`VaOscillator`); the two
wavetable tiers are independent engines with opposite goals (clean vs. lo-fi)
and don't share code with each other or with VA.

## VA — virtual-analog core

`va.hpp` is the shared bandlimited core: sine, saw, square (with pulse
width), and triangle, generated directly from the phase and corrected at
each discontinuity by composing `PhaseAccumulator` (below) with the
BLEP/BLAMP kernels. It underpins VCO and DCO — neither reimplements shape
generation, they configure and drive this class.

- **Anti-aliasing is polyBLEP, not a tabulated minBLEP** — an *improved*,
  not alias-free, correction: roughly 11-15 dB better than the trivial
  waveform below 20 kHz. A free-running sine needs no correction and is
  bit-identical to `std::sin`; a *synced* sine does step and is corrected
  like any other shape.
- **Hard sync** (`next_synced`) and **through-zero FM** (a negative
  increment) both compose with the correction structurally — a sample with
  several coincident discontinuities just sums their corrections, so sync +
  TZFM need no special-cased combination. Measured benefit is 11-34 dB over
  the trivial waveform across sync, TZFM, and the two together, but the
  benefit collapses as the instantaneous frequency approaches Nyquist: a
  synced sine gains 12 dB at a 5 kHz deviation, 6 dB at 20 kHz, and nothing
  at 60 kHz. Past that point the carrier itself is unrepresentable and no
  discontinuity correction can rescue it — oversampling the FM path is a
  separate concern this doesn't attempt.
- **Pulse width** on the square is clamped to [0, 1]; a width narrower than
  one sample period can't be represented cleanly (the two edges' corrections
  overlap), but the output stays finite and bounded rather than blowing up.

```cpp
osc.set_shape(VaShape::square);
osc.set_pulse_width(0.3);
double sample = osc.next(increment);

// Hard sync at a caller-detected reset point:
double synced = osc.next_synced(increment, sync_frac, 0.0);
```

## VCO — circuit-flavored analog character

`vco.hpp` wraps a `VaOscillator` core with the deterministic front-to-back
path of an analog voltage-controlled oscillator: a pitch-control front end
(`VcoTuning`, 1 V/octave with scale error and HF-compression knobs), an
integrator-leak "bow" on the saw ramp, a per-shape lumped waveshaper, a
level-vs-pitch tilt, and an output DC-blocking (AC-coupling) stage — plus
two seeded, deterministic pitch-noise sources layered onto the increment
before it reaches the core:

- **Drift** — a slow, bandlimited random walk (one-pole-colored white
  noise) with a corner around 0.4 Hz by default. `drift_depth` is the RMS
  pitch excursion in cents; the wander evolves over hundreds of
  milliseconds.
- **Jitter** — fast, near-white cycle-to-cycle frequency noise, independent
  per sample. `jitter_depth` is the RMS per-sample frequency deviation in
  cents.

Every stage defaults to its neutral value, so a default-constructed
`VcoOscillator` is bit-for-bit a `VaOscillator` — the "analog" behavior is
opt-in, not baked in. The noise source is seed-reproducible (`set_seed`, no
`random_device`): the same seed and inputs give bit-identical output.

```cpp
VcoOscillator vco;
vco.prepare(sample_rate);
vco.set_shape(VaShape::saw);
vco.set_bow(2.0);              // charge-curve ramp instead of linear
vco.set_drift_depth(3.0);      // 3 cents RMS slow wander
vco.set_jitter_depth(0.5);     // 0.5 cents RMS per-sample noise
vco.set_seed(12345);

double sample = vco.next(increment);
```

Be aware the bow and waveshaper are memoryless maps applied *after* the
core's own bandlimited correction — standard analog-modeling composition,
but honest about its limit: a nonlinearity on an already-bandlimited signal
reintroduces a little aliasing, same as an analog circuit's own stages do.

## DCO — divider-clocked, quantized pitch

`dco.hpp` models a late-1970s/early-1980s divider-clocked oscillator: a
crystal-derived master clock (`master_clock_hz`, default 8 MHz) is divided
down by a programmable counter, and each terminal-count reset drives the
shared VA shape stage. Unlike VCO, this front-end owns **no drift or jitter
parameter at all** — that would contradict the architecture. A DCO's
characteristic imperfection is **pitch quantization**, not drift, because an
integer divider can only realize the discrete set `f_clk / N`.

Two divider schemes, selected by `DcoProfile::divider_scheme`:

- **Integer-N** — `N = round(f_clk / f_note)`; the reset interval is
  exactly `N` master clocks, perfectly periodic in continuous time. No
  forced sync needed — the natural phase wrap *is* the divider reset.
  Quantization error grows with note frequency (doubles per octave up).
- **Fractional-N** — a `B`-bit accumulator adds a tuning word each master
  clock and resets on carry-out, so the *average* pitch can sit arbitrarily
  close to the note. That accuracy is bought with deterministic **±1-clock
  period jitter** (the reset interval alternates between `floor` and `ceil`
  clocks around the average) — the opposite quantization/jitter tradeoff of
  integer-N, and largest at *low* notes rather than high ones.

```cpp
DcoOscillator dco;
dco.prepare(sample_rate);
DcoProfile profile;
profile.master_clock_hz = 8'000'000.0;
profile.divider_scheme = DcoDivider::integer_n;
dco.set_profile(profile);
dco.set_shape(VaShape::square);
dco.set_note_hz(440.0);

double sample = dco.next();
double cents_off = dco.detune_cents();  // the quantization error, exposed
```

Reach for DCO instead of VCO when you want the crisp, drift-free character
of a divider-clocked synth voice (Juno/Jupiter-era) and the audible
"almost-but-not-quite-in-tune" quality of integer division — not a smoothly
wandering analog pitch.

## WT (modern tier) — wavetable with clean scan/morph

`wt.hpp` is a thin osc-module front-end over the shipped
`WavetableBankT`/`WavetableT` engine (`signal/wavetable.hpp`): it plays a
set of single-cycle tables with band-limited band-switching (each
`WavetableT` selects the band whose Nyquist budget covers the current
frequency and crossfades across 128 samples on a band change) and a smooth
scan across the table set. This front-end adds only what the bank doesn't
already own:

- The osc-module per-sample frequency contract (`next(increment)`, matching
  VA/VCO/DCO).
- A one-pole **scan slew** (`set_scan_time_ms`, default 5 ms) so a
  block-rate scan control doesn't zipper — this is the *clean* wavetable
  tier.

```cpp
WtOscillator wt;
wt.prepare(sample_rate);
wt.set_wavetable_set(std::move(tables));  // off the audio thread
wt.set_position(0.6);                     // slews toward this target
wt.set_scan_time_ms(8.0);

double sample = wt.next(increment);
```

Playback is for positive frequencies only — through-zero FM is VA/VCO's
domain, not the wavetable engine's. The very first frequency after
construction or `reset()` **snaps** to its band rather than crossfading, so
a fresh voice never plays an aliased fade-in from the default band.

## WT (lo-fi tier) — a dedicated variable-clock ZOH engine

`wt_lofi.hpp` is a **separate engine**, not a mode of `WtOscillator`. It
plays a raw short single-cycle table with nearest/zero-order-hold lookup at
a playback clock that tracks pitch (`fs_play = f0 · table_length`), so its
spectral-image ladder rides at `n · L · f0` and moves with the note — that
pitch-tracking image ladder *is* the lo-fi sound, and it's exactly what a
fixed-rate, band-limited, linearly-interpolated engine like `WtOscillator`
cannot produce (linear interpolation alone suppresses the first image by
~42 dB). Five mechanisms combine to give it its character:

1. **Variable-clock ZOH images** — the pitch-tracking replication described
   above.
2. **Optional bit-depth quantization** (default 8 bits) of the *stored*
   table, undithered, about zero — odd-harmonic grit, ~49.9 dB aggregate
   SNR at 8 bits.
3. **A real reconstruction stage** (oversample → lowpass → decimate) that
   keeps a supra-Nyquist fold from re-entering the band as an unwanted
   artifact, while preserving the sub-Nyquist images that are the point.
   `set_reconstruction(false)` exposes the raw naive path for A/B or for
   callers who want the rawer grit.
4. **Hard (stepped) wave-scan** — `set_scan` selects the nearest table with
   no interpolation and no slew, so crossing a table boundary is an
   instantaneous step: the classic wavetable "zipper," faithfully
   reproduced rather than smoothed away.
5. **Short-table harmonic ceiling** — a length-`L` table represents at most
   `L/2` harmonics, so low notes are intrinsically darker and high notes
   fold, a direct consequence of playing the raw table.

```cpp
LofiWtOscillator lofi;
lofi.prepare(sample_rate);
lofi.set_tables(std::move(raw_tables), /*bit_depth=*/8);
lofi.set_scan(0.5);   // hard select, no slew

double sample = lofi.next(increment);
```

This engine ships no wavetable data of its own — you supply the raw
table(s); it reproduces the *playback engine's* character, not any
particular waveform's content.

## Shared primitives

Both VA and DCO (and therefore VCO, which builds on VA) are wiring over two
lower-level headers most callers won't touch directly, but are worth
knowing if you're building a new oscillator family on top of them.

### `phase.hpp` — `PhaseAccumulator`

A phase accumulator over the unit circle `[0, 1)` that reports every
discontinuity ("event") crossed during an `advance()` — a phase wrap,
either direction — or a forced `advance_synced()` reset — with the
event's exact sub-sample position and its `phase_before`/`phase_after`
endpoints. Negative increments run the phase backward (through-zero FM);
any magnitude is accepted (multiple wraps per sample), bounded by
`max_events_per_sample` (8) with a `truncated()` flag if exceeded — the
phase itself always stays exact even when the event list is capped.
Events compose: two events landing at the same sub-sample position simply
sum, which is what makes combinations like "sync to 0 under a negative
increment" (a sync event plus a backward wrap) come out correct without
being enumerated as a special case.

### `blep.hpp` — polyBLEP/polyBLAMP kernels

The correction kernels VA (and everything built on it) uses to bandlimit a
discontinuity: **BLEP** for a step in the *value* (a saw wrap, a square
edge, a hard-sync reset), **BLAMP** for a break in the *slope* (a triangle
apex — the value stays continuous, only the derivative jumps; BLAMP is
BLEP's integral). Every kernel returns a correction to *add* to the trivial
signal, split across the sample before and after the discontinuity
(`Correction{before, after}`) — a generator needs to be able to reach back
one sample to apply the `before` term. This is a two-point polynomial
approximation of the ideal (infinite-support) BLEP, not a tabulated
minBLEP: it buys tens of dB over the trivial waveform, not the ~100 dB a
deep-floor design reaches.

## RT contract

Every family follows the same rule the rest of `pulp::signal` does:
allocation happens at setup (`set_wavetable_set`, `set_tables`,
`set_oversample_factor`, `prepare`) and must run off the audio thread;
`next()`/`next_synced()`, `reset()`, and the per-sample setters allocate
nothing, lock nothing, and perform no I/O. All classes compute in `double`
throughout; a `float` caller narrows once on store. `vco.hpp`'s `exp`/`tanh`
and `va.hpp`'s `sin` are libcalls but not allocations, locks, or I/O.

## Measuring and validating

These oscillators are validated **reference-free** (one render, no A/B pair) by the
opt-in [Audio Quality Lab](audio-quality-lab.md#oscillator-validation-reference-free):
render any engine to a WAV with `pulp-osc-render-wav` (`--engine vco|dco|wt`, `--seed`),
then reach for the click / edge-smear detector (unexpected discontinuities), the
overlapping-Allan drift-vs-jitter separation, the synthetic oscillator corpus + ratchet,
and the offline WP-4 profile fitter. See that guide's *Oscillator validation* section.
