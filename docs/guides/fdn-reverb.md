# Multirate FDN Reverb

`pulp::signal::FdnReverb` is Pulp's full-featured algorithmic reverb. It is a
true-stereo, wet-only, 16-line feedback delay network with a selectable internal
tank rate. Behind the same controls it also offers a [classic tier](#tiers) —
parallel combs into series allpasses — for when a conventional reverb at a
fraction of the cost is the right trade. Use the smaller
[`signal::Reverb`](signal-processing.md#reverb) when you need a compact
mono-in/stereo-out effect with only decay, damping, and mix controls.

The public engine is header-only:

```cpp
#include <pulp/signal/fdn_reverb.hpp>

pulp::signal::FdnReverb reverb;

// In Processor::prepare():
reverb.prepare(sample_rate, max_block_size);
reverb.set_mode(pulp::signal::fdn::Mode::hall);

// In Processor::process():
reverb.set_parameter(pulp::signal::fdn::Param::decay, 3.5);
reverb.process_block(in_left, in_right, wet_left, wet_right, num_samples);
```

`process_block()` produces wet signal only. Put a `DryWetMixer` or an equivalent
host/graph mix around it when you need a dry path. The engine reports zero
bufferable latency; `interpolation_skew_samples()` separately reports the
ratio-dependent causal interpolation bound.

## Public API

`FdnReverbT<SampleType>` is the complete engine type;
`FdnReverb` aliases `FdnReverbT<float>`.

| Method | Contract |
|---|---|
| `prepare(double sample_rate, int max_block_size)` | Allocate worst-case storage, configure the host rate, load canonical defaults, select the default tank rate, and enter a cold state. Invalid rates fall back to 48 kHz; block capacity is at least one sample. |
| `set_mode(Mode mode)` | Stamp one mode's parameter defaults and host-rate output lowpass, then apply them immediately. Preserves the current predelay and width. |
| `set_parameter(Param param, double value)` | Set a target value. Clamps to the canonical range; rounds stepped parameters. Continuous values then follow the normal smoothing path. |
| `parameter(Param param) const` | Return the requested target value, not the current smoothed value. |
| `snap_parameters()` | Copy every target to its smoothed value and push the controls immediately. A changed tank rate is configured and returned to an exact cold phase. |
| `set_eq_band(int index, const fdn::EqBand& band)` | Author one of the ten in-loop EQ bands and redesign the bank at the current tank rate. Out-of-range indices are ignored. |
| `set_active_channels(int count)` | Set the active FDN line count, clamped to 2–16, while renormalizing input fan-out and output taps. |
| `set_flux_depth_db(double db)` | Set the maximum wandering in-loop absorption depth, clamped to the engine's 0–3 dB voicing range. |
| `set_flutter(bool enabled)` | Enable or disable fractional-delay motion in the in-loop diffusion stages. |
| `set_tier(fdn::Tier tier)` / `tier() const` | Select the network (default) or the classic tier. Takes effect at the next `process_block()`, which flushes the engine it switches to. See [Tiers](#tiers). |
| `reset()` | Apply pending targets, then clear every host-rate and tank-rate state according to the cold-state contract below. |
| `latency_samples() const` | Return `0`; the engine introduces no block-buffer latency that a host can compensate. |
| `interpolation_skew_samples() const` | Return the current ratio-dependent bound from the causal four-point interpolation bridge. This is observability, not reported plugin latency. |
| `tank_rate() const` | Return the configured internal tank rate in hertz. |
| `duck_gain() const` | Return the transient ducker's most recently applied input gain in `(0, 1]`; after reset it is exactly `1`. Intended for meters, diagnostics, and acceptance tests. |
| `tank() const` | Return read-only access to the underlying `fdn::FdnTank<SampleType>`. Its telemetry supports structural validation; application code should prefer the façade's controls. |
| `process_block(const SampleType* in_left, const SampleType* in_right, SampleType* out_left, SampleType* out_right, int sample_count)` | Process true-stereo input into wet-only stereo output. Non-positive counts are no-ops; oversized calls are internally split into prepared-size chunks. |

Call `prepare()` before any other operation. Input and output pointers must
address at least `sample_count` samples. In-place processing is not part of the
documented contract; use distinct input and output buffers.

## Controls and modes

All modes run the same DSP. A mode stamps parameter defaults and selects a
host-rate output lowpass; it does not select a separate processing path.

| Mode | Character |
|---|---|
| `room` | Short, dense, relatively bright room |
| `hall` | General-purpose hall |
| `galaxy` | Long, modulated tail with Bloom and some shimmer |
| `shimmer` | Pitch-shifted, expansive tail |
| `lofi` | Low-rate, dark, driven texture |

`set_mode()` leaves `predelay` and `width` at their current values because they
are performance choices rather than part of the authored voicing.

| Parameter | Range | Meaning |
|---|---:|---|
| `decay` | 0.1–60 s | Requested broadband decay time |
| `size` | 0–1 | Scales the delay-line lengths |
| `predelay` | 0–250 ms | Delay before tank excitation |
| `damp_hi` | 0–1 | Frequency-dependent high-frequency decay |
| `damp_lo` | 0–1 | Frequency-dependent low-frequency decay |
| `diffusion` | 0–1 | Input and in-loop scattering |
| `mod` | 0–1 | Delay motion depth |
| `shimmer` | 0–1 | Energy-normalized pitch-shifted loop injection |
| `drive` | 0–1 | In-loop saturation; perceived-level makeup stays outside the loop |
| `bloom` | 0–1 | Lifts long-decay feedback toward, but never to, unity |
| `width` | 0–1 | Wet output stereo width |
| `tank_rate` | integer 0–7 | Internal rate index; see below |

The tank-rate values are indices, not rates in hertz:

| Index | Tank rate |
|---:|---:|
| 0 | 16 kHz |
| 1 | 20 kHz |
| 2 | 24 kHz |
| 3 | 32 kHz |
| 4 | 44.1 kHz |
| 5 | 48 kHz |
| 6 | 64 kHz |
| 7 | 96 kHz |

Lower rates are deliberately darker and grainier. `tank_rate` is a texture and
setup control, not a generic quality selector.

Continuous parameters smooth over 5 ms at the engine's 32-sample control
cadence. `snap_parameters()` applies all pending targets immediately, which is
appropriate after preset loading or before a deterministic offline render.
Authored in-loop EQ, flux depth, and diffusion flutter use `set_eq_band()`,
`set_flux_depth_db()`, and `set_flutter()` rather than host-automatable
parameters because changing in-loop gain can change the realized decay.

## Tiers

`fdn::Tier::network` is the engine this guide describes. `fdn::Tier::classic`
runs `ClassicReverbT` (`<pulp/signal/classic_reverb.hpp>`, also usable on its
own): eight lowpass-feedback combs per rail in parallel, four allpasses per rail
in series, all at the host rate, no resampling. It is the conventional
Schroeder/Moorer reverb, and it costs a small fraction of the network.

The tiers share the parameter set, the modes, wet-only output, and zero
latency. Each parameter means the same thing in both where both have the
machinery:

| Parameter | Classic tier |
|---|---|
| `decay` | T60. Every comb's gain follows the absorptive law `10^(-3 D / (T60 fs))`. |
| `size` | Scales comb and allpass lengths. Lengths change when parameters are snapped (`snap_parameters()`, `reset()`, `set_mode()`), not mid-stream: integer comb lengths cannot glide. |
| `predelay` | Milliseconds before the combs. |
| `damp_hi` | In-loop one-pole lowpass on the network's cutoff curve, its loss scaled per comb by length. |
| `diffusion` | Allpass coefficient, `diffusion x 0.7`. |
| `width` | Mid/side width of the wet output. |
| `damp_lo`, `mod`, `shimmer`, `drive`, `bloom`, `tank_rate` | Stored, no effect. |

Expect the classic tier's density to be a classic reverb's: its echo-density
curve reaches the Gaussian plateau later than the network's (around 200 ms
against about 60–80 ms at the defaults), and with damping fully off it plateaus
below it, because nothing in a comb smears each recirculation.

Under automation the classic tier smooths each continuous control through
three one-pole stages at the 32-sample control rate, then moves the comb
gains, damping and width to each tick's values in per-sample steps. A sweep of
`decay` or `damp_hi` with targets set once per 128-sample host block leaves no
measurable energy at the control rate or the block rate around a probe tone;
the allpass coefficient and predelay still change at the tick.

A tier change is a clean flush, like a tank-rate change.

In the Forge catalog the five `reverb.*` nodes carry the tier as a thirteenth
baked parameter, `tier` (node-local id 13, a stepped `fdn` / `classic`
choice, default `fdn`), appended after the engine's twelve so every existing
patch keeps rendering the network bit for bit.

## Lifecycle and real-time contract

`prepare(host_rate, max_block_size)` allocates every delay and scratch buffer
for the worst supported tank rate, and the classic tier's buffers. After
`prepare()`, parameter setters, `snap_parameters()`, `reset()`,
`process_block()`, live tank-rate changes, and tier changes do not allocate.

`reset()` has a strong cold-state contract:

- It first applies every pending parameter target.
- It clears the multirate bridge, predelay, pre-diffusion, tank, ensemble,
  ducker, input and output filters, telemetry, and control cadence.
- It snaps delay lengths to their targets and atomically re-derives
  length-dependent decay, damping, and stability state.
- The next render therefore matches a freshly prepared engine with the same
  settings and deterministic modulation phase.

A tank-rate change is intentionally a hard flush, not a crossfade. The change
is applied at a processing chunk boundary before either resampler leg runs, and
the current tail is discarded. Do not automate `tank_rate` as a performance
macro. If a preset load changes the rate, call `snap_parameters()` so the new
rate and all other pending controls become one cold state before rendering.

## Architecture and ownership

The public façade coordinates the host-rate and tank-rate domains. Internal
headers under `pulp/signal/fdn/` own one kind of DSP state each:

| Module | Responsibility |
|---|---|
| `fdn_reverb.hpp` | Public lifecycle, smoothing, wet-output chain, and host/tank coordination |
| `fdn/config.hpp` | Structural constants, tuning data, parameter ranges, and mode tables |
| `fdn/multirate.hpp` | Host-to-tank and tank-to-host conversion plus interpolation phase |
| `fdn/tank.hpp` | Delay network, feedback ordering, control-derived state, and stability normalization |
| `fdn/diffusion.hpp` | Input and in-loop allpass cascades |
| `fdn/modulation.hpp` | Deterministic sine and mean-reverting delay motion |
| `fdn/loop_eq.hpp` | Authored EQ and absorptive flux filters |
| `fdn/shimmer.hpp` | Granular pitch shifting and energy-normalized injection |
| `fdn/stages.hpp` | Transient ducking, output ensemble, and limiter helpers |
| `fdn/filter_state.hpp` | Shared coefficient-driven recursive biquad state for the scalar component APIs |
| `fdn/sanitize.hpp` | Topology-neutral finite-value recovery |
| `classic_reverb.hpp` | The classic tier |
| `detail/simd_lanes.hpp` | Four-lane float / two-lane double vectors for the block kernels |

## Block processing

The engine runs the tank a control period (32 tank samples) at a time,
because controls are constant inside a period and every loop in the tank is
far longer than one:

- Each line reads its whole span along time — a contiguous Hermite read when
  the span shares one integer delay — then runs its four in-loop allpasses as
  one fused pass along time. Every read lands on a sample written before the
  span began; `FdnTank::process_block()` splits a span wherever that would
  not hold.
- Damping, flux, the decay gains, the 16-point Hadamard, and the input fan-out
  run one sample at a time across all sixteen lines as four 4-lane vectors,
  with their recursive state held in registers for the span.
- The writes are committed after the span.

The bridge carries both rails through each resampling filter as one two-lane
double vector, with the recursion rearranged so it is one multiply-add deep.
Delay modulation and the ensemble's LFOs are rotation oscillators re-anchored
from an exact double phase every 4096 samples instead of a `sin()` per line per
sample.

Precision follows the cost of being wrong: delay positions, modulation, the
bridge filters, and the flux biquads stay double precision; the delay lines,
diffusion, damping, and the matrix are single precision, as the ring buffers
always were. The block engine reproduces the per-sample formulation's output to
within float rounding (its null residual sits around -105 dB) and its envelope,
T60, and mixing time exactly — see `test_fdn_reverb_block_engine.cpp`.

Every per-sample expression is evaluated the same way whatever span it falls
in, and recursive state is snapped and sanitized on fixed sample grids
counted from reset, never at caller block boundaries. A render is therefore
bit-identical however the host divides it, down to one-sample blocks.

Rings that advance in lockstep (the sixteen lines, the in-loop allpass stages)
are staggered inside their allocations: equal-sized rings written at equal
offsets otherwise land on the same cache sets and low address bits, and loads
stall behind unrelated stores.

Keep these boundaries when extending the engine:

- The façade owns cross-stage lifecycle order; a stage owns its own state and
  reset implementation.
- The tank owns the meaning of smoothed tank controls. The façade owns target
  values, smoothing, and host/tank rate transitions.
- Modes and parameter ranges are data in `config.hpp`, not branches copied into
  the engine or catalog wrapper.
- Anything that can add feedback-loop energy must participate in the tank's
  worst-case gain normalization. Loudness compensation belongs after the
  feedback tap.
- Host/catalog identifiers and parameter injection belong in
  `pulp/host/forge_fdn_reverb_catalog.hpp`; the signal engine must not learn
  about graph node IDs or `BakedParamView`.
- Generic signal utilities must not depend on the FDN. An internal helper
  should move into a general `pulp::signal` module only when another processor
  has a real use for the same contract.

## Finite-state recovery

The FDN uses defense in depth because one corrupt recursive line can spread
through the Hadamard matrix to all sixteen lines. Recovery stays local, but the
block kernels place the guards at the boundaries rather than on every
multiply-add:

- Input is sanitized where it enters each bridge leg; the scalar component
  APIs (`AllpassStage::process`, `FluxBank::process`, `LoopEq::process`,
  `ButterworthState`, `TransientDucker::process`) keep their per-sample
  `fdn::SanitizedBiquadState` / `fdn::finite_or_zero()` guards.
- The damping stage clamps its input to the sanity ceiling, so the damping,
  flux, and allpass recursions see finite, bounded input; each recursion is
  then bounded by construction.
- Every recursive state (damping, flux, ducker envelopes, bridge and wet
  filters) is snapped to zero and zeroed if non-finite every 32 samples of its
  own clock — often enough that no state at the snap threshold can reach the
  denormal range, and on a grid that never depends on host blocks.
- The post-Hadamard boundary clamps to its tighter ceiling before fan-out; the
  delay write boundary kills non-finite values, applies the wider emergency
  ceiling, and snaps.
- Equal-rate and resampled bridge paths follow the same non-finite recovery
  policy.

Do not replace these local guards with output-only sanitization. By the time a
bad value reaches the stereo sum, recursive state may already be contaminated.

## Validation ownership

Changes to this engine have two focused owners:

- `pulp-test-fdn-reverb` proves DSP structure and behavior, including
  stability, decay, bandwidth, reset/rate-switch parity, finite recovery,
  determinism, and drive distortion. `test_fdn_reverb_block_engine.cpp` pins
  the block engine to the per-sample formulation's output envelope, T60, and
  mixing time, each with a negative control, and proves block-partition
  exactness for every mode; `test_classic_reverb.cpp` covers the classic tier.
- `pulp-test-fdn-reverb-catalog` proves the production host wrapper, parameter
  propagation, lifecycle behavior, allocation-free reset/rate switching, and
  the tier parameter: its default renders the network exactly, and `classic`
  renders the classic engine.

The registrations live together in `test/cmake/fdn_reverb_tests.cmake`. Run the
focused Release targets after changes:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
tools/ci/governed-build.sh cmake --build build --target pulp-test-fdn-reverb \
  pulp-test-fdn-reverb-catalog
./build/test/pulp-test-fdn-reverb
./build/test/pulp-test-fdn-reverb-catalog
```

Cost is measured, not gated: `pulp-dsp-throughput-benchmark --filter reverb`
(configure with `-DPULP_BENCHMARK=ON`) reports the `reverb.fdn` and
`reverb.classic` rows at 32/128/512-sample blocks.

Behavioral claims should be tested at the narrowest observable layer. For
example, prove public drive propagation from tank telemetry and measure
saturator distortion at the tank output, where downstream makeup and limiting
cannot create a false positive. Pair reset/rate-switch comparisons with an
explicit all-finite check so a non-finite difference cannot masquerade as
parity.
