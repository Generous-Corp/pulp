# Advanced DSP API

This reference is the complete public-method inventory for Pulp's advanced DSP
families. It complements the generated [C++ API reference](../api-reference.md):
the guide groups methods by authoring responsibility while Doxygen provides the
template declarations and source-linked definitions.

Unless stated otherwise, use the float alias (`Foo`); `Foo64` is the same API
with `double` samples. Call `prepare()` and any method that loads or sizes data
off the audio thread. After preparation, the listed `process*()` calls and
ordinary scalar setters are allocation-free. `reset()` clears state;
`discard_history()` is the constant-time fault-recovery form where provided.

## Reading the method contracts

This page uses the following compact signature notation so the complete API can
remain scannable without hiding argument or return contracts:

| Notation | Exact meaning |
|---|---|
| `set_x(value)` | `void set_x(T value)`, where `T` is the named enum/boolean/integer or the processor's `SampleType`. Finite numeric inputs are clamped to the stated domain. A non-finite input is rejected or replaced by the documented safe default; it never becomes persistent DSP state. |
| `x()` | A const inspector returning the named control's effective, clamped plain-domain value. It does not allocate or mutate audio state. |
| `prepare(sample_rate, ...)` | Non-RT lifecycle call. It validates the rate, sizes all bounded storage, derives coefficients, and leaves the instance ready to process. Repeat it after the sample rate or a prepared capacity changes. |
| `reset()` | RT-safe deterministic state clear after preparation unless the class-specific note says otherwise. It preserves configuration and prepared capacity. |
| `discard_history()` | Constant-time RT-safe hostile-input recovery. It drops delay/history state without a potentially capacity-sized clear. |
| `process(...)` / `process_block(...)` | RT-safe, allocation-free after `prepare()`. Pointer arguments name caller-owned contiguous buffers; `frames` must fit the prepared capacity when one exists. In-place use is supported only where the class exposes an in-place overload. |
| `latency_samples()` | Integer host/PDC latency at the current prepared topology. Artistic predelay and evolving modulation delay are not host latency unless explicitly stated. |
| Other inspectors | Const snapshots or pure calculations in the units named by the suffix (`_db`, `_hz`, `_ms`, `_samples`, `_pct`). References remain instance-owned and are invalidated by the lifecycle/configuration operation named in the class note. |

The ranges below are the public plain domains, not normalized host values.
`[0, 1]` and `[0, 100]` are deliberately distinct. Enum setters accept only
their declared enumerators. Unless called out as a topology operation, controls
may be changed between blocks and are consumed without allocation on the next
processing call.

## Filters and crossovers

### `LinkwitzRileyCrossoverT<SampleType, MaxBands>`

This fixed-capacity LR4 crossover creates between two and `MaxBands` ordered
bands. `prepare(sample_rate, cutoffs)` fixes the band count; cutoffs are plain Hz,
finite, strictly increasing, below Nyquist, and inside the numerically supported
coefficient domain reported by `supports_configuration()`. The template uses a
double-precision recursive realization independently of its floating-point API
sample type; validation also rejects degenerate coefficients and poles too
close to the unit circle.
`set_cutoffs(cutoffs, transition_samples)` preserves topology. A nonzero
transition moves one topology-preserving-transform bank through logarithmically
interpolated, bilinear-warped cutoff design values for the exact sample count and
rejects an overlapping retune. Its integrator state is not reinterpreted when
coefficients move. Downward moves must also meet the logarithmic slew floor
reported by `minimum_transition_samples()`. The public parameter-rate guarantee
is at most `maximum_downward_log_slew_nepers_per_second()` in the logarithm of
the bilinear-warped cutoff; it is not an input-independent signal peak bound,
because peaks also depend on recursive state established by prior input.
`set_cutoffs()` rejects shorter or numerically unrepresentable transitions
without changing the live configuration. `max()` from
`minimum_transition_samples()` is reserved as the invalid or unrepresentable
sentinel and is never an accepted transition length. Upward moves may use any
nonzero length whose entire rounded trajectory is representable. Before a
transition becomes live, configuration bounds the accumulated multiplication
roundoff and the endpoint correction implied by the rounded multiplier. The
endpoint correction may be at most two scheduled logarithmic steps and, for a
downward move, must also remain inside the public 20-neper/second rate. This
rejects extremely long transitions even when their multiplier differs from
unity, because that fact alone does not prove that repeated multiplication will
arrive near the target.
The transcendental endpoint and slew calculations happen in `set_cutoffs()`;
`process()` uses bounded multiply/add/divide arithmetic. It does not crossfade
differently phased banks. A zero-length transition is an explicit immediate
coefficient change and clears recursive state. During a transition, `cutoff()`
continues to report the last stationary cutoff set until the target becomes
live. Invalid configurations are rejected without changing the live
configuration. A non-finite sample clears recursive state, returns zero bands,
and increments `fault_count()` so the following finite sample starts recovered.

Earlier bands receive the all-pass response of every later split. Summing every
band therefore reconstructs a flat magnitude response with zero host latency.
`band_response()` and `reconstruction_response()` expose the exact stationary
complex response for plotting and verification without running audio; response
queries outside `[0, Nyquist]` are rejected rather than folded or clamped. The
historical two-band `LinkwitzRileyT` API remains available; its two-argument
coefficient design retains the rounded Q used by existing renders, while
`set_frequency_precise()` selects the exact Butterworth value for new work.

- Lifecycle: `prepare(sample_rate, cutoffs)`, `reset()`.
- Controls: `set_cutoffs(cutoffs, transition_samples)`.
- Processing: `process(input)` returns `Frame{bands, count, healthy}`.
- Inspection: `supports_configuration()`, `minimum_transition_samples()`, `maximum_downward_log_slew_nepers_per_second()`, `band_count()`, `cutoff_count()`, `cutoff()`, `sample_rate()`, `transitioning()`, `healthy()`, `fault_count()`, `latency_samples()`, `band_response()`, `reconstruction_response()`.

## Dynamics

### Shared dynamics contract

`<pulp/signal/dynamics_contract.hpp>` publishes `EnvelopeFollowerT`,
`StereoEnvelopeFollowerT`, and `GainReduction`. Followers consume raw signed
samples in the linear amplitude domain. Peak mode rectifies; RMS mode squares,
smooths, and square-roots. Their attack and release controls are milliseconds
measured exactly from 10 to 90 percent of the smoothed state: amplitude in peak
mode and mean-square power in RMS mode. `BallisticsFilterT` retains its legacy
nominal 2.2 exponent for render compatibility; `EnvelopeFollowerT` selects the
exact ln(9) convention. `current()` returns linear amplitude,
`current_db()` returns dBFS with a configurable floor, and the coefficient
accessors expose the exact pure ballistics intermediate used by processing.

`GainReduction::db()` is always a non-negative attenuation magnitude; positive
infinity represents a complete mute and has zero linear gain.
`from_signed_db()` adapts processors whose legacy meter is a negative gain;
`from_magnitude_db()` adapts positive attenuation meters. Every compressor
lineage exposes `gain_reduction()` using this convention without changing the
sign or behavior of its existing `gain_reduction_db()` method. `Compressor`,
`Limiter`, and `NoiseGate` expose the same telemetry contract.
### `TruePeakLimiter`

`prepare(sample_rate, channels, params)` fixes the explicit channel count and
allocates the look-ahead and monotonic-peak queues off the audio thread. The
plain control domains are ceiling `[-24, 0]` dBTP, look-ahead `[0, 20]` ms,
release `[5, 2000]` ms, and `ChannelLink::{linked,independent}`. Look-ahead and
link policy are topology controls supplied at prepare time; ceiling and release
may change between blocks through `set_ceiling_dbtp()` and `set_release_ms()`.

The detector reconstructs four phases with a 129-tap, beta-10.5 Kaiser-windowed
sinc interpolator. Its causal linear-phase delay is 64 base-rate samples. The
limiter reserves a 0.20 dB detector guard; the test gate compares it with an
independent 16x, 257-tap, beta-14 polyphase sinc oracle over phase-shifted
near-Nyquist, multitone, impulse, and planted sample-peak material. The reported
host latency and tail are exactly `64 + ceil(lookahead_ms * sample_rate / 1000)`
base-rate samples. The ceiling is a reconstructed-signal contract over the
documented detector/oracle domain, not merely a clamp on stored samples.

Linked mode applies the maximum peak across channels without moving the stereo
image; independent mode maintains one peak queue and gain envelope per channel.
`gain_reduction_db(channel)` is the current non-negative attenuation magnitude.
A non-finite input clears bounded history, emits a zero frame, and increments
`fault_count()`.

- Lifecycle and topology: `prepare()`, `reset()`, `prepared()`, `channel_count()`, `channel_link()`.
- Controls: `set_ceiling_dbtp()`, `ceiling_dbtp()`, `lookahead_ms()`, `set_release_ms()`, `release_ms()`.
- Processing: `process_frame()`, `process_interleaved()`.
- Host and telemetry: `latency_samples()`, `tail_samples()`, `gain_reduction_db()`, `fault_count()`.
- Detector inspection: `interpolation_factor()`, `interpolation_taps()`, `detector_latency_samples()`, `detector_guard_db()`, `maximum_supported_sample_rate()`, `maximum_lookahead_ms()`, `detector_phases()`.

### `FeedforwardCompressor`

`prepare(double sample_rate, double max_lookahead_ms)` fixes the maximum delay
capacity; `set_lookahead_ms(SampleType)` is then clamped to
`[0, max_lookahead_ms]` and changes reported latency without allocating.
Threshold/makeup/knee are dB, ratio is `[1, 100]`, attack/release and RMS window
are positive milliseconds, detector is `DetectorMode::{peak,rms}`, and the
program-dependent, auto-makeup, and stereo-link arguments are booleans. Scalar
`process(SampleType)` returns one compressed sample; stereo/block overloads
mutate their supplied channels. Curve methods take an input level in dB and
return output/gain in dB; `gain_reduction_db()` is the current non-negative meter.

- Lifecycle: `prepare(sample_rate, max_lookahead_ms)`, `reset()`.
- Controls: `set_threshold_db()`, `set_ratio()`, `set_knee_width_db()`, `set_attack_ms()`, `set_release_ms()`, `set_detector()`, `set_rms_window_ms()`, `set_lookahead_ms()`, `set_program_dependent_release()`, `set_makeup_gain_db()`, `set_auto_makeup()`, `set_stereo_link()`.
- Processing: `process()`, `process_stereo()`, `process_block()`, `process_block_stereo()`.
- Inspection: `detector()`, `latency_samples()`, `static_curve_db()`, `gain_computer_db()`, `effective_makeup_db()`, `gain_reduction_db()`, `gain_reduction()`.

### `VcaCompressor`

`prepare(double sample_rate)` allocates the fixed lookahead capacity.
`set_threshold_db`, `set_knee_db`, `set_makeup_db`, and `set_ceiling_db` use dB;
`set_ratio` uses `[1,20]`; `set_time_ms` is `[1,500]`; `set_mix` is `[0,1]`.
`set_negative_ratio_mode(bool)` enables the infinity-plus branch and
`set_neg_ratio_amount(SampleType)` selects its negative slope. Lookahead changes
the current latency within the prepared capacity. `process(SampleType)` returns
one sample and `process_block(SampleType*, int)` mutates a mono buffer. Curve,
level, coefficient, and gain inspectors are read-only snapshots in their suffix units.

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_threshold_db()`, `set_ratio()`, `set_negative_ratio_mode()`, `set_neg_ratio_amount()`, `set_knee_db()`, `set_time_ms()`, `set_attack_release_ratio_k()`, `set_makeup_db()`, `set_lookahead_ms()`, `set_mix()`, `set_ceiling_db()`.
- Processing: `process()`, `process_block()`.
- Curve and meter inspection: `latency_samples()`, `static_curve_db()`, `gain_computer_db()`, `gain_computer_unclamped_db()`, `active_ratio()`, `gain_reduction_db()`, `gain_reduction()`, `level_db()`, `mean_square()`, `current_gain_linear()`, `attack_coef()`, `release_coef()`.

### `DiodeBridgeCompressor`

`prepare(double sample_rate)` fixes the 4x ADAA/oversampling topology.
Threshold, knee, makeup and measured curve methods use dB; ratio is `[1.5,20]`,
attack/release are positive milliseconds, character is `[0,1]`, mix is
`[0,100]`, and sidechain HPF is `[20,400]` Hz. `set_feedback(bool)` selects the
detection topology and `set_adaa(bool)` is a measurement/quality switch; change
either while stopped. `process(SampleType)` returns the processed sample and the
block form mutates its buffer. Circuit-stage methods return currents,
resistances, or transfer values and never expose owned mutable state.

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_threshold_db()`, `set_ratio()`, `set_knee_db()`, `set_attack_ms()`, `set_release_ms()`, `set_makeup_db()`, `set_character()`, `set_mix_percent()`, `set_sc_hpf_hz()`, `set_auto_release()`, `set_feedback()`, `set_adaa()`.
- Processing: `process()`, `process_block()`.
- Inspection: `latency_samples()`, `worst_case_gain()`, `gain_reduction_db()`, `gain_reduction()`, `control_drive()`, `static_curve_db()`, `static_curve_feedback_db()`.

`DiodeBridgeGain` additionally provides `prepare()`, `reset()`, `set_character()`,
`set_adaa()`, `drive()`, `control_drive_for_current()`, `dynamic_resistance()`,
`control_drive_for_gain_db()`, `gain_for_control_drive()`, `curvature()`,
`max_operating_amplitude()`, `shape()`, `shape_antiderivative()`, `third_harmonic_ratio()`,
and `process()`. `TransformerBracket` provides `prepare()`, `reset()`,
`set_character()`, `set_adaa()`, `saturate()`, `saturate_antiderivative()`, and
`process()` for callers that need the exposed circuit stages independently.

### `FetCompressor`

`prepare(double sample_rate)` fixes the oversampled feedback loop. Input/output
gain and knee are dB, attack is microseconds, release is milliseconds,
transformer amount and mix are `[0,1]`, and `set_ratio(FetRatio)` accepts the
five hardware-style ratio positions including all-buttons-in. `process()`
returns one sample; the block form mutates mono storage. Measured/static curve
methods accept dB input and return dB output/reduction; circuit and bound
inspectors return immutable instantaneous values used for meters and validation.

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_input_gain_db()`, `set_output_gain_db()`, `set_ratio()`, `set_attack_us()`, `set_release_ms()`, `set_knee_db()`, `set_transformer_amount()`, `set_mix()`.
- Processing: `process()`, `process_block()`.
- Configuration and curve inspection: `ratio()`, `latency_samples()`, `sample_rate()`, `oversampled_rate()`, `static_curve_db()`, `gain_computer_db()`, `measured_static_curve_db()`, `measured_gain_reduction_db()`, `loop_slope()`, `measured_ratio()`, `measured_knee_db()`, `nominal_ratio()`, `effective_knee_db()`, `bias_shift_db()`, `coloration_depth()`, `attack_coefficient()`, `release_coefficient()`.
- Circuit, bound, and meter inspection: `gain_reduction_db()`, `gain_reduction()`, `control_voltage()`, `divider_conductance()`, `divider_small_signal_gain()`, `divider_gain()`, `coloration_multiplier()`, `coloration_multiplier_bound()`, `control_for_reduction_db()`, `divider_supremum_is_provable()`, `resampler_peak_gain_bound()`, `worst_case_gain()`.

## Nonlinear and tone

### `Saturator`

`prepare(double sample_rate)` sizes the optional 2x path. `set_shape(Shape)` and
`set_alias_policy(AliasPolicy)` select fixed algorithms; switch alias policy
while stopped because it changes latency. Drive is `[-12,36]` dB, bias is
`[-1,1]`, pre/de-emphasis corners are `0` (off) or `[20,8000]` Hz, pre-boost is
`[0,18]` dB, tone tracking is boolean, mix is `[0,1]`, and trim is `[-24,24]`
dB. `process(SampleType)` advances state and returns audio; `shaped(SampleType)`
is the pure memoryless transfer used by plots/tests. Inspectors return effective
controls, host latency, or a conservative linear gain bound.

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_shape()`, `set_drive_db()`, `set_bias()`, `set_tone_pre_hz()`, `set_tone_tracking()`, `set_tone_de_hz()`, `set_pre_boost_db()`, `set_alias_policy()`, `set_mix()`, `set_output_trim_db()`.
- Processing and pure transfer: `process()`, `shaped()`.
- Inspection: `shape()`, `drive_db()`, `bias()`, `alias_policy()`, `latency_samples()`, `worst_case_gain()`.

### Circuit clippers and tone stack

All three `prepare(double sample_rate)` calls derive coefficients off the audio
thread. Diode model/topology setters take their enums; symmetry is `[-1,1]`;
resistance/capacitance are strictly positive physical values; tone corners are
positive Hz, pre-gain is dB, and tone mix is `[0,1]`. `process`, `process_pre`,
and `process_post` consume/return one sample. Solver iteration/residual/voltage
and topology/gain inspectors are const diagnostics and do not advance the DSP.

- `DiodeClipper`: `prepare()`, `set_diode_model()`, `set_symmetry()`, `set_resistance()`, `set_capacitance()`, `reset()`, `last_iteration_count()`, `process()`, `voltage()`, `resistive_residual()`.
- `FeedbackClipper`: `prepare()`, `set_topology()`, `topology()`, `set_diode_model()`, `set_symmetry()`, `set_feedback_resistance()`, `set_input_resistance()`, `set_knee_corner_hz()`, `linear_gain()`, `reset()`, `last_iteration_count()`, `process()`.
- `ToneStack`: `prepare()`, `set_pre_tone_hz()`, `set_post_tone_hz()`, `set_pre_gain_db()`, `set_tone_mix()`, `reset()`, `process_pre()`, `process_post()`.

### `FuzzPair`

`prepare(double sample_rate)` sizes its optional oversampler. Device is
`FuzzDevice::{silicon,germanium}`, fuzz/bias-starve/mix are `[0,1]`,
source impedance is positive kOhm, and output level is dB. Oversampling and
drift toggles are topology/validation choices and should change while stopped;
`set_seed(uint32_t)` makes drift deterministic. `process()` returns one sample
and `process_block()` mutates mono storage. Electrical inspectors return the
effective modeled operating point; `latency_samples()` follows oversampling.

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_device()`, `set_fuzz()`, `set_bias_starve()`, `set_source_impedance_kohm()`, `set_output_level_db()`, `set_mix()`, `set_oversampling_enabled()`, `set_seed()`, `set_drift_enabled()`.
- Processing: `process()`, `process_block()`.
- Inspection: `device()`, `loading_factor()`, `bias_voltage()`, `base_bias_voltage()`, `quiescent_collector()`, `stage_gain()`, `input_scale_volts()`, `available_current()`, `loop_gain()`, `worst_residual()`, `latency_samples()`.

### `TapeMachine`

`prepare(double sample_rate)` derives record/repro and gap filters. Archetype,
speed, and EQ curve are coherent machine configuration and should change
between blocks (or while stopped when a host cannot tolerate coefficient
rebuilds). Speed is one of the machine-supported ips values, bias is `[-1,1]`,
drive/age/mix are `[0,1]`, crosstalk and print-through are negative dB, and
print offset is positive ms. `set_print_through(SampleType db, SampleType ms,
bool pre_echo)` is the three-argument topology call; pre-echo affects latency.
`process(const SampleType* in_l, const SampleType* in_r, SampleType* out_l,
SampleType* out_r, int frames)` writes stereo output. EQ/FIR references remain
owned by the machine and are invalidated by prepare or machine reconfiguration.

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_archetype()`, `set_speed_ips()`, `set_eq_curve()`, `set_bias()`, `set_drive()`, `set_age()`, `set_crosstalk_db()`, `set_companding_enabled()`, `set_print_through()`, `set_mix()`.
- Processing: `process(in_l, in_r, out_l, out_r, frames)`.
- Control inspection: `archetype()`, `speed_ips()`, `eq_curve()`, `effective_bias()`, `drive()`, `age()`, `crosstalk_db()`, `companding_enabled()`, `print_through_db()`, `print_offset_ms()`, `pre_echo_enabled()`.
- Design and host inspection: `latency_samples()`, `oversampler_latency_samples()`, `worst_case_insertion_gain()`, `record_eq()`, `playback_eq()`, `gap_fir()`, `reproduce_gap_m()`, `reproduce_alignment_db()`, `sample_rate()`.

### `SpeakerModel`

`prepare(double sample_rate)` derives the physical/filter topology. Driver and
box setters take their enums; volume is positive litres, resonance trim is
semitones, resonance Q is non-negative, breakup/compression/diffraction and mic
position are normalized amounts, treble is Hz, drive/trim are dB, mic distance
is cm, and axis is degrees. Scalar `process(SampleType)` returns mono audio; the
buffer overload writes `frames` samples. Response/physics inspectors return the
effective derived frequency, Q, gain, excursion, or enum value and never rebuild
the model.

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls: `set_driver_archetype()`, `set_box_type()`, `set_box_volume_l()`, `set_resonance_trim_semitones()`, `set_q_resonance()`, `set_cone_breakup_amount()`, `set_treble_rolloff_hz()`, `set_drive_db()`, `set_compression_amount()`, `set_mic_distance_cm()`, `set_mic_position_pct()`, `set_mic_axis_deg()`, `set_diffraction_amount()`, `set_output_trim_db()`.
- Processing: `process(sample)`, `process(in, out, frames)`.
- Response and bound inspection: `latency_samples()`, `worst_case_gain()`, `compliance_ratio()`, `resonance_fc_hz()`, `resonance_q()`, `resonance_peak_db()`, `resonance_peak_hz()`, `baffle_step_hz()`, `ripple_hz()`, `dipole_hz()`, `breakup_mode()`, `breakup_mode_hz()`, `offaxis_corner_hz()`, `proximity_gain_db()`, `presence_shelf_db()`, `air_loss_db()`, `inductance_magnitude_db()`, `bl_beta()`, `cms_gamma()`, `excursion()`, `dynamic_fc_hz()`, `archetype()`, `archetype_index()`, `box_type()`, `sample_rate()`.

## Modulation effects

### `PhaserStages`

`prepare(double sample_rate)` sizes the maximum stage bank. Stage count is an
even integer in `[4,12]`; rate/center are positive Hz, depth and mix are
`[0,100]`, feedback is `[-0.98,0.98]`, stereo spread is `[0,0.5]`, stagger is a
positive ratio, and wave is `LfoWave`. `set_seed(uint32_t)` controls stochastic
waves. Stereo `process(left,right)` returns a sample pair; `process_mono(sample)`
returns mono. `sweep_frequency_hz(channel)` is the current realized all-pass corner.

- Lifecycle: `prepare(sample_rate)`, `reset()`.
- Controls and paired accessors: `set_stage_count()`/`stage_count()`, `set_rate_hz()`/`rate_hz()`, `set_depth()`/`depth()`, `set_center_hz()`/`center_hz()`, `set_feedback()`/`feedback()`, `set_mix()`/`mix()`, `set_stereo_spread()`/`stereo_spread()`, `set_wave()`/`wave()`, `set_stagger_ratio()`/`stagger_ratio()`, plus `set_seed()`.
- Processing and observation: `process()`, `process_mono()`, `latency_samples()`, `worst_case_gain()`, `sweep_frequency_hz()`, `notch_count()`, `notch_frequency_hz()`, `notch_frequency_analog_hz()`.

### Vibrato family

Every `prepare(double sample_rate)` call sizes its state; stage-count changes on
`PhaseVibrato` are prepared topology and should occur while stopped. Rates and
centers are positive Hz, delay/fade are ms, cents are non-negative pitch depth,
normalized depths/mixes are `[0,100]`, and UniVibe mode is
`UniVibeMode::{vibrato,chorus}`. Each scalar `process(SampleType)` returns one
sample. Delay history makes `DelayVibrato::discard_history()` the bounded fault
path; the filter variants clear fixed stage state in `reset()`. Delay/base/corner
inspectors return realized samples, envelope, or Hz without advancing phase.

- `DelayVibrato`: `prepare()`, `set_rate_hz()`, `rate_hz()`, `set_depth_cents()`, `depth_cents()`, `set_delay_ms()`, `set_fade_in_ms()`, `base_delay_samples()`, `modulation_amplitude_samples()`, `depth_envelope()`, `latency_samples()`, `reset()`, `discard_history()`, `process()`.
- `PhaseVibrato`: `prepare()`, `set_rate_hz()`, `rate_hz()`, `set_depth()`, `depth()`, `set_center_hz()`, `center_hz()`, `set_stage_count()`, `stage_count()`, `set_mix()`, `mix()`, `corner_hz()`, `latency_samples()`, `reset()`, `process()`.
- `UniVibe`: `prepare()`, `set_rate_hz()`, `rate_hz()`, `set_depth()`, `depth()`, `set_mode()`, `mode()`, `control()`, `corner_scale()`, `stage_corner_hz()`, `latency_samples()`, `reset()`, `process()`.

### `ChorusEnsemble`

`prepare(double sample_rate)` sizes delay/BBD history. Voicing and Juno mode are
enums; BBD color is boolean; rate is positive Hz; depth, mix, and width are
`[0,100]`. Treat voicing/BBD switches as between-block configuration. Stereo
`process(SampleType* left, SampleType* right, int frames)` mutates both channels.
Voice/delay/BBD inspectors are current immutable design values; gain/filter-L1
methods are conservative validation bounds.

- Lifecycle: `prepare()`, `reset()`, `discard_history()`.
- Controls: `set_voicing()`, `set_juno_mode()`, `set_rate_hz()`, `set_depth()`, `set_mix()`, `set_stereo_width()`, `set_bbd_color()`.
- Processing: `process()`.
- Inspection: `voicing()`, `juno_mode()`, `rate_hz()`, `bbd_color()`, `latency_samples()`, `current_delay_ms()`, `voice_count()`, `calibration()`, `juno_spec()`, `bbd_bandwidth_hz()`, `bbd_stage_delay_ms()`, `worst_case_gain()`, `shelf_l1()`, `highpass_l1()`.

### `Flanger`

`prepare(double sample_rate)` sizes the maximum delay engine. Mode, polarity,
delay engine, and waveform are enums and are between-block topology/character
choices. Rate/barber-pole shift are Hz; depth/center/offset are ms; feedback and
mix use the class's normalized public range; spread is degrees. Scalar
`process(SampleType)` returns mono and `process_stereo(SampleType&,SampleType&)`
mutates a pair. Delay inspectors return the currently realized time/samples, not
additional host latency.

- Lifecycle: `prepare()`, `reset()`, `discard_history()`.
- Controls and accessors: `set_mode()`/`mode()`, `set_polarity()`/`polarity()`, `set_delay_engine()`/`delay_engine()`, `set_rate_hz()`, `set_waveform()`, `set_stereo_spread()`, `set_depth_ms()`/`depth_ms()`, `set_center_delay_ms()`/`center_delay_ms()`, `set_offset_ms()`/`offset_ms()`, `set_feedback()`/`feedback()`, `set_mix()`/`mix()`, `set_barberpole_shift_hz()`/`barberpole_shift_hz()`.
- Processing and inspection: `process()`, `process_stereo()`, `latency_samples()`, `worst_case_gain()`, `effective_depth_ms()`, `instantaneous_delay_ms()`, `fixed_delay_samples()`, `mix_gains()`, `notch_hz()`, `notch_spacing_hz()`.

### `SsbFrequencyShifter`

`prepare(double sample_rate)` builds the quadrature network and feedback delay.
Shift is signed Hz, feedback is `[0,0.98]`, delay is positive ms, mode is
`ShiftMode`, mix and stereo spread are `[0,100]`. Scalar `process()` returns one
shifted sample; `process_stereo(left,right)` returns/mutates the stereo result.
The Hilbert network's fixed group delay is returned by `latency_samples()`;
feedback delay remains an artistic loop time.

- Lifecycle: `prepare()`, `reset()`, `discard_history()`.
- Controls and accessors: `set_shift_hz()`/`shift_hz()`, `set_feedback()`/`feedback()`, `set_feedback_delay_ms()`/`feedback_delay_ms()`, `set_mode()`/`mode()`, `set_mix()`, `set_stereo_spread()`.
- Processing, mappings, and host contract: `process()`, `process_stereo()`, `latency_samples()`, `worst_case_gain()`, `shift_hz_from_knob()`, `knob_from_shift_hz()`, `feedback_delay_ms_from_knob()`.

### `LeslieRotary` and `ScannerVibrato`

Both `prepare(double sample_rate)` calls allocate delay/reflection storage.
Leslie speed is its enum; rotor targets/crossover/reflection corner are Hz,
acceleration/deceleration are seconds, radii/distances are metres, angle is
degrees, delay terms are ms, gains/depth/drift are in their suffix domains, and
reflection count is a bounded integer. `set_seed(uint32_t)` makes drift
repeatable. Scalar and block overloads either return one sample/pair or mutate
the named buffers. Rotor phase/rate/delay inspectors are live snapshots.
Scanner mode is its enum, scan is Hz, line length is ms, tap fractions and
chorus mix are normalized; its depth/pitch inspectors describe the realized scan.

- `LeslieRotary` lifecycle and mode: `prepare()`, `reset()`, `discard_history()`, `set_speed()`, `speed()`.
- `LeslieRotary` controls: `set_horn_fast_hz()`, `set_horn_slow_hz()`, `set_drum_fast_hz()`, `set_drum_slow_hz()`, `set_horn_accel_s()`, `set_horn_decel_s()`, `set_drum_accel_s()`, `set_drum_decel_s()`, `set_crossover_hz()`, `set_horn_radius_m()`, `set_drum_radius_m()`, `set_mic_distance_m()`, `set_mic_angle_deg()`, `set_am_depth()`, `set_dir_depth_db()`, `set_drum_dir_depth_db()`, `set_dir_corner_hz()`, `set_d_bias_ms()`, `set_reflection_db()`, `set_num_reflections()`, `set_refl_delay_ms()`, `set_refl_spacing_ms()`, `set_refl_corner_hz()`, `set_drift_cents()`, `set_seed()`, `set_mix()`.
- `LeslieRotary` inspection and processing: `latency_samples()`, `horn_rate_hz()`, `drum_rate_hz()`, `target_horn_hz()`, `target_drum_hz()`, `horn_phase()`, `drum_phase()`, `mic_face_offset()`, `horn_delay_seconds()`, `drum_delay_seconds()`, `worst_case_delay_samples()`, both `process()` overloads, and both `process_block()` overloads.
- `ScannerVibrato`: `prepare()`, `reset()`, `discard_history()`, `set_mode()`, `mode()`, `set_scan_hz()`, `set_line_ms()`, `set_v1_frac()`, `set_v2_frac()`, `set_v3_frac()`, `set_chorus_mix()`, `depth_fraction()`, `dry_mix()`, `latency_samples()`, `peak_pitch_shift_ratio()`, `worst_case_delay_samples()`, `process()`, `process_block()`.

## Pitch, time, and granular

### `PitchShifter`

`prepare(double sample_rate)` sizes the maximum window. Source, pedal mode, and
interpolator are enums; direct/heel/toe/harmony/dive values are semitones;
detune is cents; pedal, mix, drift and detent controls are normalized; glide and
window are ms. Window/interpolator changes affect quality/latency and should be
made between blocks. `process(SampleType)` returns dry/wet output and
`process_wet(SampleType)` returns only the shifted path. Target/current ratio,
phase, window and latency inspectors are immutable snapshots/pure mappings.

- Lifecycle: `prepare()`, `reset()`, `discard_history()`.
- Shift controls and accessors: `set_shift_source()`/`shift_source()`, `set_shift_semitones()`/`shift_semitones()`, `set_pedal()`/`pedal()`, `set_pedal_mode()`/`pedal_mode()`, `set_targets()`, `set_harmony()`, `set_detune_cents()`, `set_dive_floor_semis()`, `set_window_ms()`/`window_ms()`, `set_glide_ms()`, `glide_up_ms()`, `glide_down_ms()`, `set_mix()`/`mix()`, `set_detents()`/`detents()`, `set_interp()`/`interp()`, `set_drift_depth()`/`drift_depth()`, `snap_to_target()`.
- Processing: `process()`, `process_wet()`.
- Pure and live inspection: `default_mix_for()`, `mode_uses_detents()`, `dc_blocker_magnitude_peak()`, `target_semitones()`, `pedal_law()`, `current_semitones()`, `current_ratio()`, `warble_hz()`, `tap_phase_pi()`, `window_samples()`, `latency_samples()`.

### `YinTracker`

`prepare(double sample_rate)` allocates the analysis window;
`set_f0_range(SampleType min_hz, SampleType max_hz)` requires positive ordered
bounds and rebuilds the prepared analysis geometry, so call it off the audio
thread. `process(SampleType)` consumes one mono sample and returns no audio.
`f0_hz()` is meaningful only when `voiced()` is true; tau/window/integration
values are samples, `min_cmnd()` is confidence/error, and cost is MAC/sample.

`prepare()`, `reset()`, `discard_history()`, `set_f0_range()`, `f0_min_hz()`,
`f0_max_hz()`, `process()`, `f0_hz()`, `tau_samples()`, `voiced()`, `min_cmnd()`,
`latency_samples()`, `hop_samples()`, `window_samples()`, `integration_samples()`, `tau_min()`,
`tau_max()`, and `cost_mac_per_sample()`.

### `HarmonyEngine` and `DiatonicMap`

`HarmonyEngine::prepare(double sample_rate)` sizes tracker and two shifters.
Key/scale/off-scale policy/interpolator are enums; voice index is `0` or `1`;
intervals are scale degrees/semitones as documented by `DiatonicMap`, detune and
humanize are cents, levels are dB, and glide/crossfade are ms. Enable is boolean.
`process(SampleType)` returns the aligned dry-plus-voices sample. Mapping/voice
inspectors return immutable per-voice decisions and component latency.
`DiatonicMap::map_midi(SampleType)` and `map_hz(SampleType)` are pure mappings;
degree access requires an index in `[0, degree_count())`.

- `HarmonyEngine` lifecycle: `prepare()`, `reset()`, `discard_history()`.
- Mapping and voice controls: `set_key()`, `set_scale()`, `set_off_scale_policy()`, `set_voice_interval()`/`voice_interval()`, `set_voice_detune_cents()`/`voice_detune_cents()`, `set_voice_level_db()`/`voice_level_db()`, `set_voice_enabled()`/`voice_enabled()`, `set_dry_level_db()`/`dry_level_db()`, `set_glide_ms()`/`glide_ms()`, `set_humanize_cents()`/`humanize_cents()`, `set_crossfade_ms()`/`crossfade_ms()`, `set_interp()`.
- Processing and inspection: `process()`, `latency_samples()`, `tracker_latency_samples()`, `shifter_latency_samples()`, `tracked_f0_hz()`, `voiced()`, `voice_mapping()`, `voice_cents()`, `voice_ratio()`, `voice_shift_semitones()`, `mute_gain()`, `tracker()`, `diatonic_map()`.
- `DiatonicMap`: `set_key()`/`key()`, `set_scale()`/`scale()`, `set_off_scale_policy()`/`off_scale_policy()`, `degree_count()`, `degree_semitone()`, `map_midi()`, `map_hz()`.

### `CyclicStretch`

`prepare(double sample_rate, ...)` fixes capture/grain capacity. Cycle is Hz,
grain periods are positive, crossfade/mix are `[0,100]`, stretch is a positive
ratio, capture is ms, output is dB, and shape/regime are enums. Regime/capture
changes are prepared topology and belong off the audio thread. `process()`
returns one sample. Schedule positions/counts are monotonically evolving
diagnostics in samples; returned gain is a conservative linear bound.

- Lifecycle: `prepare()`, `reset()`.
- Controls: `set_cycle_hz()`, `set_grain_periods()`, `set_crossfade_pct()`, `set_crossfade_shape()`, `set_stretch_ratio()`, `set_capture_ms()`, `set_mix()`, `set_output_db()`, `set_regime()`.
- Processing: `process()`.
- Resolved design and live schedule: `cycle_samples()`, `grain_samples()`, `crossfade_samples()`, `hop_samples()`, `flutter_hz()`, `capture_window_samples()`, `sample_rate()`, `stretch_ratio()`, `crossfade_shape()`, `latency_samples()`, `worst_case_gain()`, `schedule_input_position()`, `grain_input_position()`, `grain_count()`, `next_grain_out_pos()`, `read_position()`, `total_captured()`.

### `GranularEngine`

`prepare(double sample_rate, ...)` fixes ring and grain capacity.
`set_buffer(const SampleType* data, size_t samples, int channels)` publishes
caller-owned immutable storage; keep it alive until stopped or replaced.
`write_live(...)` appends to the owned ring. Position/stretch/coherence/mix and
sprays are normalized unless suffixed ms/semitones/dB/Hz; maximum grains is
bounded by prepared capacity; source/window/interpolation/steal policy are enums;
seed is `uint32_t`. Both `process()` overloads write/return generated audio
without allocation. `grain(index)` is instance-owned diagnostic state valid
until the next scheduler mutation; all other scheduler/window values are snapshots.

- Lifecycle and source: `prepare()`, `reset()`, `set_source()`, `source()`, `set_buffer()`, `write_live()`.
- Grain controls and accessors: `set_stretch()`/`stretch()`, `set_position()`/`position()`, `set_position_spray_ms()`/`position_spray_ms()`, `set_density_hz()`/`density_hz()`, `set_grain_ms()`/`grain_ms()`, `set_async_jitter()`/`async_jitter()`, `set_max_grains()`/`max_grains()`, `set_steal_policy()`/`steal_policy()`, `set_window_taper()`/`window_taper()`, `set_window_trapezoid()`/`window_trapezoid()`, `set_pitch_semitones()`/`pitch_semitones()`, `set_pitch_spray_semitones()`, `set_pan_spray()`, `set_coherence()`/`coherence()`, `set_interp()`/`interp()`, `set_level_db()`, `set_mix()`/`mix()`, `set_seed()`/`seed()`.
- Processing: both `process()` overloads.
- Bounds, window, and scheduler inspection: `latency_samples()`, `mean_overlap()`, `grain_gain()`, `window_mean()`, `window_rms()`, `window_at()`, `active_grain_count()`, `grain()`, `grain_index()`, `steal_count()`, `clamp_count()`, `ring_length()`, `ring_storage_sample()`, `causality_guard_samples()`, `derived_guard_samples()`.

## Synthesis and sequencing

### `AdditiveBank`

`prepare(double sample_rate, int max_partials)` fixes oscillator capacity.
Fundamental is positive Hz, partial count is `[1,max_partials]`, inharmonicity is
non-negative, tilt is dB/octave, master is dB, morph is `[0,1]`, attack/release
are ms, detune is cents, and pitch glide uses target Hz plus ms. Voice,
envelope-mode, spectral-domain and retrigger-phase arguments are their enums;
seed is `uint32_t`. `load_voice(const VoiceTable&)` and envelope setters copy
prepared design data and belong off the audio thread. `next()` returns one
sample; `process(out,frames)` replaces the output buffer. Partial/envelope
methods are pure realized-frequency/gain queries.

- Lifecycle and note events: `prepare()`, `reset()`, `retrigger()`, `release()`, `active()`.
- Voice controls and accessors: `set_fundamental_hz()`/`fundamental_hz()`, `set_partial_count()`/`partial_count()`, `max_partials()`, `set_inharmonicity_b()`/`inharmonicity_b()`, `set_spectral_tilt_db_oct()`/`spectral_tilt_db_oct()`, `set_master_gain_db()`/`master_gain_db()`, `load_voice()`/`voice()`, `set_partial()`.
- Envelope and variation controls: `set_envelope_a()`, `set_envelope_b()`, `set_morph()`/`morph()`, `set_spectral_domain()`/`spectral_domain()`, `envelope_db_at()`, `set_envelope_mode()`/`envelope_mode()`, `set_attack_ms()`, `set_release_ms()`, `set_detune_cents()`/`detune_cents()`, `doublet_active()`, `set_pitch_glide()`, `set_retrig_phase()`/`retrig_phase()`, `set_seed()`.
- Processing and realized pitch: `next()`, `process()`, `latency_samples()`, `worst_case_gain()`, `partial_frequency()`, `partial_frequency_hz()`, `nyquist_guard_gain()`.
- `SpectralEnvelope`: `clear()`, `size()`, `add()`, `tilt()`, `gain_db_at()`. `VoiceTable`: `clear()`, `add()`.

### `Vocoder`

`prepare(double sample_rate)` builds the maximum analysis/synthesis bank.
Band count is bounded by the prepared maximum; band low/high and internal pitch
are positive Hz; pulse width/noise/sibilance/unvoiced sensitivity/dry-wet are
normalized; attack/release are ms; formant shift is semitones; trim is dB; and
carrier/wave are enums. `process(modulator, carrier_ext, out_dry)` consumes two
samples, writes the aligned dry sample by reference, and returns wet audio.
Band/filter/envelope inspectors return const current coefficients or values;
indices must be in `[0, band_count())`.

- Lifecycle and host contract: `prepare()`, `reset()`, `latency_samples()`.
- Bank and carrier controls: `set_band_count()`, `set_band_range_hz()`, `set_carrier_source()`/`carrier_source()`, `set_internal_wave()`, `set_internal_pulse_width()`, `set_internal_pitch_hz()`/`internal_pitch_hz()`, `set_noise_mix()`.
- Envelope and output controls: `set_attack_ms()`, `set_release_ms()`, `set_unvoiced_sensitivity()`, `set_sibilance_mix()`, `set_formant_shift_semitones()`, `set_formant_freeze()`/`formant_freeze()`, `set_output_trim_db()`, `set_dry_wet()`.
- Processing: `process(modulator, carrier_ext, out_dry)`.
- Realized-bank inspection: `band_count()`, `band_ratio()`, `band_q()`, `section_q()`, `bands_per_octave()`, `shift_bands()`, `band_center_hz()`, `attack_eff_ms()`, `release_eff_ms()`, `analysis_band()`, `band_envelope()`, `synthesis_gain()`, `unvoiced()`, `zcr_hz()`, `zcr_window_ms()`.

### Modular sequencing

All `prepare(double sample_rate)` calls derive thresholds/timing; all `reset()`
methods restore deterministic seed/state, while `apply_reset_edge(bool)` handles
a live reset event once. `process` consumes level/edge inputs and returns or
updates the documented CV/gate state without allocation. Stage/grid/register
sizes and indices are integer-bounded; pitch/CV/range values are volts or
semitones as named; slide/refractory values are ms; probabilities/duty are
`[0,100]`; masks/seeds/registers are unsigned integers; direction, access,
quantizer mode, gate operation, and gate mode are enums. Inspectors return the
last emitted stage/cell/register/step/draw result and never consume a new edge.

- `StageSeq`: `prepare()`, `set_num_stages()`/`num_stages()`, `set_direction()`/`direction()`, `set_stage_pitch()`/`stage_pitch()`, `set_stage_pulse_count()`/`stage_pulse_count()`, `set_stage_gate_mode()`/`stage_gate_mode()`, `set_stage_slide()`/`stage_slide()`, `set_stage_skip()`/`stage_skip()`, `set_slide_ms()`/`slide_ms()`, `set_repeat_duty()`/`repeat_duty()`, `set_seed()`, `apply_reset_edge()`, `reset()`, `latency_samples()`, `gate()`, `pitch_v()`, `stage()`, `pulse()`, `started()`, `process()`.
- `CartesianWalk`: `prepare()`, `set_size()`, `width()`, `height()`, `set_value()`, `value()`, `set_access()`, `access()`, `set_offsets()`, `apply_reset_edge()`, `reset()`, `latency_samples()`, `x()`, `y()`, `cell_x()`, `cell_y()`, `gate()`, `cv()`, `process()`.
- `Rungler`: `prepare()`, `set_reg_bits()`/`reg_bits()`, `set_dac_bits()`/`dac_bits()`, `set_feedback_tap()`/`feedback_tap()`, `set_range_v()`/`range_v()`, `set_external_data()`/`external_data()`, `set_seed_pattern()`/`seed_pattern()`, `apply_reset_edge()`, `reset()`, `latency_samples()`, `register_bits()`, `dac_code()`, `value()`, `process()`.
- `QuantizeScale`: `prepare()`, `set_mode()`/`mode()`, `set_edo()`/`edo()`, `set_scale_mask()`/`scale_mask()`, `set_root_pc()`/`root_pc()`, `set_hysteresis_cents()`/`hysteresis_cents()`, `apply_reset_edge()`, `reset()`, `latched_step()`, `process()`.
- `ProbGate`: `prepare()`, `set_probability()`/`probability()`, `set_seed()`, `apply_reset_edge()`, `reset()`, `latency_samples()`, `draw_count()`, `process_edge()`, `process()`.
- `GateLogic`: `prepare()`, `set_op()`/`op()`, `apply_reset_edge()`, `reset()`, `latency_samples()`, both `process()` overloads, `process_levels()`.

## Space and convolution

### `NonlinAmbience`

`prepare(double sample_rate)` allocates both topology banks.
`set_topology(const NonlinTopology&)` performs coherent design work and must run
off the audio thread; `request_topology(const NonlinTopology&)` publishes that
request to the bounded hosted swap path. Program is an enum; length/predelay are
ms; density/gate/attack/width/mix are `[0,100]`; growth/diffusion/converter are
normalized design amounts; tone is `[-1,1]`; damping is Hz; output is dB; seed
is `uint32_t`. `process_sample(left,right)` mutates a pair and block `process`
mutates stereo buffers. Tap references are instance-owned and invalidated by a
topology rebuild; swap/work counters are atomic/read-only diagnostics.

- Lifecycle: `prepare()`, `reset()`.
- Topology: `set_program()`, `set_length_ms()`, `set_predelay_ms()`, `set_density_pct()`, `set_density_growth()`, `set_gate_hold_pct()`, `set_attack_pct()`, `set_topology()`, `request_topology()`.
- Color and output: `set_seed()`, `set_diffusion()`, `set_tone()`, `set_hf_damp_hz()`, `set_width_pct()`, `set_converter_amount()`, `set_output_gain_db()`, `set_mix_pct()`.
- Processing: `process_sample()`, `process()`.
- Topology, response, and bound inspection: `latency_samples()`, `topology_rebuild_count()`, `topology_work_units_last_sample()`, `tap_count()`, `tap()`, `tap_norm()`, `window_samples()`, `predelay_samples()`, `allpass_length()`, `worst_case_gain()`, `program()`, `length_ms()`, `tone()`, `swap_in_progress()`, `envelope()`.

### `ZeroLatencyConvolver`

`prepare(double sample_rate, int max_block, int channels)` fixes all scheduler
storage. `load_impulse_response(const SampleType* const* channels, int
channel_count, int samples, double ir_sample_rate)` copies, resamples and
publishes an IR and is non-RT. Gain/trim are dB; predelay/fade are ms;
wet/dry/width are `[0,100]`; cuts are Hz; normalize mode is an enum; true-stereo
is boolean; taps-per-phase is a positive quality integer. `process(in,out,frames)`
accepts `frames <= max_block`, permits only the documented channel aliasing, and
reports zero host latency. Prepared-IR references and level schedule values are
instance-owned until the next load/prepare; cost is the last block's diagnostic.

- Lifecycle and IR publication: `prepare()`, `load_impulse_response()`, `reset()`.
- Controls: `set_ir_gain_db()`, `set_predelay_ms()`, `set_true_stereo()`, `set_wet_percent()`, `set_dry_percent()`, `set_width_percent()`, `set_lowcut_hz()`, `set_highcut_hz()`, `set_normalize_mode()`, `set_tail_trim_db()`, `set_tail_fade_ms()`, `set_resample_taps_per_phase()`.
- Processing: `process()`.
- Host and bound inspection: `latency_samples()`, `worst_case_gain()`, `l1_norm()`, `is_loaded()`, `sample_rate()`, `normalize_mode()`, `tail_trim_db()`, `tail_fade_ms()`, `predelay_ms()`, `predelay_samples()`.
- Prepared IR and scheduler inspection: `head_length()`, `num_levels()`, `level_block_length()`, `level_ir_start()`, `level_partitions()`, `level_margin()`, `prepared_ir_length()`, `prepared_ir_channels()`, `prepared_ir()`, `last_block_cost()`.

## Shared public primitives

These lower-level types are public because custom processors, editors, and tests
may need to compose or inspect the same stages as the complete effects.

- `HilbertQuadratureNetwork`: `reset()` and `process()` expose the quadrature
  network used by `SsbFrequencyShifter`.
- `junction::JunctionPair`: `theta()`, `current()`, `conductance()`,
  `antiderivative()`, `conduction_estimate()`, `knee_voltage()`,
  `adaa_current()`, and `adaa_conductance()` expose the shared junction law
  used by the circuit processors.
- `TapeEqSection`: `set()`, `reset()`, `process()`, and `response_db()` expose a
  tape EQ stage and its pure response. `TapeCompander`: `prepare()`, `reset()`,
  `encode()`, and `decode()` expose the paired companding stages.
  `tape::EqTimeConstants::has_bass_shelf()` reports whether a preset includes
  the low-frequency shelf.
- `TransportEdge`: `prepare()`, `set_refractory_ms()`, `set_thresholds()`,
  `reset()`, `latency_samples()`, `process(run, reset, clock)`, and
  `process(run, reset)` convert signal-domain transport lanes into one shared
  set of edges.
- `VactrolConditioner`: `prepare()`, `set_rise_ms()`, `set_fall_ms()`,
  `rise_ms()`, `fall_ms()`, `reset()`, `control()`, and `process()` expose the
  asymmetric control lag used by optocoupler effects.

The split compatibility headers `dynamics_core.hpp`, `slew_limiter.hpp`, and
`trigger_kit.hpp` preserve the public include surface for the documented
dynamics and [modulation toolkit](modulation-toolkit.md) types; they do not add
separate processor classes. `nonlin_ambience_design.hpp` and
`zero_latency_convolver_support.hpp` expose the value types and pure design
functions used by their complete processors; Doxygen lists those free-function
signatures alongside the class methods above.
