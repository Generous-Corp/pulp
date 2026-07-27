# Analog VCF

Pulp's analog VCF has two layers:

- `AnalogVcfT` is the normal choice for an instrument. It maps panel-domain
  cutoff and resonance controls through measured Juno, Jupiter-8, Prophet-5,
  and Minimoog laws, then configures compensation, headroom, cross-modulation,
  drift, and the shared filter engine as one voicing.
- `OtaCascadeFilterT` is the lower-level, voicing-neutral four-pole nonlinear
  cascade. Use it when you need to design those control laws and gain stages
  yourself or select a non-24 dB output mode.

Both classes use fixed storage and allocate no memory during configuration or
processing. They are safe to call on the audio thread when the instance is
owned by that thread. They do not synchronize concurrent mutation: publish
controls to the audio thread rather than calling setters concurrently from a
UI thread.

## Recipe 1: a measured Juno-style filter

Keep the processor alive across blocks so its filter, smoothing, oversampler,
and deterministic drift state remain continuous.

```cpp
#include <pulp/signal/analog_vcf.hpp>

class JunoVoiceFilter {
public:
    void prepare(double sample_rate) noexcept {
        filter_.set_sample_rate(sample_rate);
        filter_.set_oversampling(2);
        filter_.set_voicing(pulp::signal::AnalogVcf::Voicing::juno);
        filter_.set_smoothing_time_ms(3.0);
        filter_.reset();
    }

    void set_controls(float cutoff, float cutoff_mod_octaves,
                      float resonance, float drive_db) noexcept {
        filter_.set_parameters(cutoff, cutoff_mod_octaves,
                               resonance, drive_db);
    }

    void process(float* samples, int count) noexcept {
        filter_.process(samples, count);
    }

    int latency_samples() const noexcept {
        return filter_.latency_samples();
    }

private:
    pulp::signal::AnalogVcf filter_;
};
```

`cutoff` and `resonance` are panel positions in `[0, 1]`, not Hz and Q.
`cutoff_mod_octaves` is added in octave space and clamps to `[-16, 16]`.
`drive_db` clamps inside the engine to `[-96, 72]` dB. `set_parameters()` is
the efficient sample-accurate path when all four controls arrive together.

Choose a `Voicing` for its complete measured control law, not merely a label:

| Voicing | Modelled control-law family |
|---|---|
| `juno` | Juno panel cutoff and resonance laws over the OTA cascade |
| `jupiter` | Jupiter-8 panel cutoff and resonance laws over the OTA cascade |
| `prophet5` | Prophet-5 cutoff/resonance law with its compensation, headroom, and nonlinear character |
| `minimoog` | Minimoog cutoff/resonance law with its makeup, headroom, cross-modulation, and drift character |

The default is `juno`, cutoff `0.5`, zero cutoff modulation, zero resonance,
zero drive, 44.1 kHz, 2x oversampling, and zero smoothing time. Setting sample
rate or oversampling resets DSP history; changing voicing does too. Ordinary
control changes do not reset it.

## Recipe 2: design a custom cascade voicing

Use `OtaCascadeFilterT` when your design owns the pole-frequency mapping,
feedback ceiling, saturation, compensation, and output law.

```cpp
#include <pulp/signal/ota_cascade_filter.hpp>

class CustomCascadeFilter {
public:
    void prepare(double sample_rate) noexcept {
        filter_.set_sample_rate(sample_rate);
        filter_.set_oversampling(4);
        filter_.set_mode(pulp::signal::OtaCascadeFilter::Mode::lowpass24);
        filter_.set_pole_frequency(1200.0);
        filter_.set_resonance(0.65);
        filter_.set_k_max(4.1);
        filter_.set_drive_db(6.0);
        filter_.set_saturation_headroom(1.25);
        filter_.set_compensation(0.4);
        filter_.set_smoothing_time_ms(3.0);
        filter_.reset();
    }

    float process(float input) noexcept {
        return filter_.process(input);
    }

    int latency_samples() const noexcept {
        return filter_.latency_samples();
    }

private:
    pulp::signal::OtaCascadeFilter filter_;
};
```

The low-level defaults are 44.1 kHz, 2x oversampling, a 1 kHz pole, zero
resonance, feedback ceiling `4.3`, `lowpass24`, unity drive/headroom/output,
zero bias/compensation/cross-modulation/drift, and zero smoothing time.

`Mode` selects which cascade tap combination reaches the output:
`lowpass24`, `lowpass12`, `highpass4`, or `bandpass4`. These modes reuse one
four-pole nonlinear feedback topology; they are not separate filter objects.

## Oversampling, latency, and graph PDC

The valid oversampling factors are `1`, `2`, `4`, `8`, and `16`. Passing any
other value to `set_oversampling()` selects 2x. `OtaCascadeFilterT` resets on an
effective factor change; `AnalogVcfT::set_oversampling()` resets on every call.
Latency is always reported in base-rate graph samples:

| Factor | Latency |
|---:|---:|
| 1x | 0 samples |
| 2x | 32 samples |
| 4x | 48 samples |
| 8x | 56 samples |
| 16x | 60 samples |

`latency_samples_for_oversampling(factor)` returns the same table without an
instance and returns `-1` for an invalid factor. `latency_samples()` reports
the current instance's value.

When the Forge Analog VCF adapter is registered as a `CustomNodeType`, it
declares the 2x value as fixed base-rate latency. A prepared `SignalGraph`
then includes that value in host latency reporting and delay-compensates
parallel branches. Custom node latency is prepare-stable: changing a type's
oversampling/latency contract requires re-registration and re-prepare, plus a
type-version bump when persisted graphs must distinguish the old timing.

## Reset and real-time behavior

- `reset()` clears pole and oversampling history, restores deterministic drift
  state, and snaps smoothed coefficients to their current targets.
- `set_sample_rate()` resets both layers. `OtaCascadeFilterT` resets on an
  effective oversampling-factor change; `AnalogVcfT::set_oversampling()` resets
  on every call. `AnalogVcfT::set_voicing()` also resets when the voicing changes.
- The in-place `process(buffer, count)` overload is a no-op for a null buffer or
  non-positive count.
- Setter arguments are clamped as documented below. No setter, reset, or
  process method allocates.
- Do not reconstruct or reset the filter at block boundaries; doing so destroys
  history and can create discontinuities.

## `AnalogVcfT` API

| API | Contract |
|---|---|
| `AnalogVcfT()` | Constructs the defaults described above and clears DSP state. |
| `Voicing` | `juno`, `jupiter`, `prophet5`, or `minimoog`; default `juno`. |
| `set_sample_rate(rate)` | Uses at least 64 Hz, rebuilds the measured voicing, and resets history. |
| `set_oversampling(factor)` | Accepts 1/2/4/8/16; invalid values select 2x. The high-level voicing is reapplied and history resets on every call. |
| `set_voicing(voicing)` | Applies the complete measured law. A change resets history; setting the current value is a no-op. |
| `set_cutoff(knob01, modulation_octaves = 0)` | Clamps the panel knob to `[0, 1]` and modulation to `[-16, 16]` octaves, then updates the voicing law. |
| `set_resonance(knob01)` | Clamps the panel control to `[0, 1]` and updates the voicing law. |
| `set_drive_db(db)` | Sets input drive; the engine clamps it to `[-96, 72]` dB. |
| `set_parameters(cutoff, modulation, resonance, drive)` | Transactional form of the four setters, with the same units and clamps; avoids duplicate voicing-law work for sample-accurate control. |
| `set_smoothing_time_ms(ms)` | Sets coefficient smoothing; negative values become zero. |
| `process(input)` | Processes and returns one sample while advancing filter, oversampler, smoothing, and drift state. |
| `process(buffer, count)` | Processes `count` samples in place; null/non-positive input is a no-op. |
| `reset()` | Clears history and restores deterministic drift state without changing controls. |
| `latency_samples()` | Returns current fixed oversampling latency in base-rate samples. |
| `latency_samples_for_oversampling(factor)` | Returns the table above, or `-1` for an invalid factor. Does not require an instance. |
| `cutoff_hz()` | Returns the requested table-derived corner in Hz. The realised corner may differ because the engine clamps its pole to `[20 Hz, 0.45 * sample_rate]` (higher at the floor or lower at the ceiling). |
| `requested_cutoff_hz_for(voicing, knob, modulation = 0)` | Returns the requested Hz readout without an instance; clamps knob to `[0, 1]` and modulation to `[-16, 16]`. |
| `oversampling()` | Returns the active factor. |
| `voicing()` | Returns the active measured voicing. |

Use `AnalogVcf` for `AnalogVcfT<float>` and `AnalogVcf64` for
`AnalogVcfT<double>`.

## `OtaCascadeFilterT` API

| API | Contract |
|---|---|
| `OtaCascadeFilterT()` | Constructs the low-level defaults described above and snaps coefficients to them. |
| `Mode` | Selects `lowpass24`, `lowpass12`, `highpass4`, or `bandpass4`; default `lowpass24`. |
| `set_sample_rate(rate)` | Clamps to at least 64 Hz, updates internal rate and coefficients, and resets history. |
| `set_oversampling(factor)` | Accepts 1/2/4/8/16; invalid values select 2x. An effective change resets history. |
| `set_pole_frequency(hz)` | Clamps the pole to `[20 Hz, 0.45 * sample_rate]`. |
| `set_resonance(normalized)` | Clamps normalized feedback control to `[0, 1]`. |
| `set_k_max(maximum)` | Sets the non-negative feedback ceiling used by normalized resonance. |
| `set_drive_db(db)` | Sets input drive, clamped to `[-96, 72]` dB. |
| `set_saturation_headroom(amount)` | Sets nonlinear-junction headroom, clamped to `[0.25, 16]`. |
| `set_bias(amount)` | Sets asymmetric junction bias, clamped to `[-2, 2]`. |
| `set_compensation(amount)` | Sets resonance-dependent passband compensation, clamped to `[0, 1]`; high-pass mode intentionally uses unity compensation. |
| `set_output_gain(gain)` | Sets a non-negative linear output multiplier. |
| `set_cross_modulation(depth, drive = 2)` | Sets output cross-modulation depth in `[-1, 1]` and nonlinear drive in `[0.1, 16]`. |
| `set_mode(mode)` | Selects the output tap combination and updates compensation. |
| `set_smoothing_time_ms(ms)` | Sets coefficient smoothing; negative values become zero. |
| `set_drift(cutoff_cents, resonance_fraction, cutoff_rate_hz = 1.5, resonance_rate_hz = 0)` | Clamps cutoff drift to `[0, 100]` cents, resonance drift to `[0, 0.1]`, cutoff rate to `[0.05, 20]` Hz, and a positive independent resonance rate to `[0.05, 40]` Hz. A non-positive resonance rate follows the cutoff rate. Zero depths disable drift. |
| `process(input)` | Processes and returns one base-rate sample, including oversampling, smoothing, and drift advancement. |
| `process(buffer, count)` | Processes `count` samples in place; null/non-positive input is a no-op. |
| `reset()` | Clears pole/oversampler history, restores the fixed drift seed, and snaps coefficients to current targets. |
| `latency_samples_for_oversampling(factor)` | Returns the base-rate table above, or `-1` for an invalid factor. |
| `latency_samples()` | Returns current fixed oversampling latency in base-rate samples. |
| `oversampling()` | Returns the active factor. |
| `sample_rate()` | Returns the active, minimum-clamped base sample rate. |

Use `OtaCascadeFilter` for `OtaCascadeFilterT<float>` and
`OtaCascadeFilter64` for `OtaCascadeFilterT<double>`.
