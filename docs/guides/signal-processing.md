# Signal Processing

The `pulp::signal` namespace provides 20 DSP processors for use inside `Processor::process()`. All are real-time safe after initialization. Include individual headers or use the convenience header:

```cpp
#include <pulp/signal/signal.hpp>  // everything
#include <pulp/signal/biquad.hpp>  // just biquad
```

All processors live in `core/signal/include/pulp/signal/`.

---

## 1. Utilities

### SmoothedValue

Linearly interpolates toward a target value over a configurable ramp time. Use for parameter smoothing to avoid zipper noise.

```cpp
SmoothedValue<float> gain_smooth;

// In prepare():
gain_smooth.set_ramp_time(0.02f, sample_rate);  // 20ms ramp

// In process():
gain_smooth.set_target(new_gain);  // begins ramping
for (int i = 0; i < num_samples; ++i)
    output[i] = input[i] * gain_smooth.next();
```

| Method | Description |
|---|---|
| `set_ramp_time(seconds, sample_rate)` | Set smoothing duration. Minimum 1 sample. |
| `set_target(value)` | Begin ramping toward value. |
| `set_immediate(value)` | Jump to value instantly (no ramp). |
| `next()` | Return next interpolated sample and advance. |
| `skip(n)` | Advance by n samples without returning values. |
| `is_smoothing()` | True if ramp is still in progress. |
| `current()` | Current value without advancing. |
| `target()` | Target value. |

Template parameter: `T` (default `float`). Also works with `double`.

**Sample rate dependency:** `set_ramp_time()` must be called with the current sample rate.

---

### Gain

Applies gain in linear or dB scale. Includes `db_to_linear()` and `linear_to_db()` free functions.

```cpp
signal::Gain gain;
gain.set_gain_db(-6.0f);

// Per-sample:
float out = gain.process(input_sample);

// Per-buffer:
gain.process(buffer, num_samples);  // in-place
```

| Method | Description |
|---|---|
| `set_gain_db(float db)` | Set gain in decibels. |
| `set_gain_linear(float linear)` | Set gain as linear multiplier. |
| `gain_db()` | Current gain in dB. |
| `gain_linear()` | Current gain as linear multiplier. |
| `process(float)` | Process one sample, returns result. |
| `process(float*, int)` | Process buffer in-place. |

Free functions: `db_to_linear(float db)` and `linear_to_db(float linear)`.

**Sample rate dependency:** None.

---

### DryWetMixer

Crossfades between dry and wet signals. Mix 0.0 = fully dry, 1.0 = fully wet.

```cpp
signal::DryWetMixer mixer;
mixer.set_mix(0.5f);  // 50/50

// Per-sample:
float out = mixer.process(dry_sample, wet_sample);

// Per-buffer:
mixer.process(dry_buf, wet_buf, output_buf, num_samples);
```

| Method | Description |
|---|---|
| `set_mix(float)` | Set mix ratio. Clamped to [0, 1]. |
| `mix()` | Current mix value. |
| `process(float dry, float wet)` | Mix one sample pair. |
| `process(dry*, wet*, out*, int)` | Mix buffers. |

Default mix: 1.0 (fully wet).

**Sample rate dependency:** None.

---

### Panner

Equal-power stereo panner. Pan -1.0 = full left, 0.0 = center, +1.0 = full right.

```cpp
signal::Panner panner;
panner.set_pan(0.3f);  // slightly right

// Mono to stereo:
auto [left, right] = panner.process(mono_sample);

// Stereo balance adjustment:
float l = left_sample, r = right_sample;
panner.process(l, r);  // modifies in-place
```

| Method | Description |
|---|---|
| `set_pan(float)` | Set pan position. Clamped to [-1, 1]. |
| `pan()` | Current pan value. |
| `process(float)` | Pan mono to stereo, returns `StereoSample{left, right}`. |
| `process(float& left, float& right)` | Adjust stereo balance in-place. |

Uses cosine/sine panning law for constant power.

**Sample rate dependency:** None.

---

## 2. Envelopes

### Adsr

ADSR envelope generator. Call `note_on()` / `note_off()`, then `next()` per sample. Supports retriggering (does not reset level on note_on, ramps from current position).

```cpp
signal::Adsr env;
env.set_sample_rate(sample_rate);
env.set_params({
    .attack  = 0.01f,  // seconds
    .decay   = 0.1f,   // seconds
    .sustain = 0.7f,   // level 0-1
    .release = 0.3f,   // seconds
});

env.note_on();
for (int i = 0; i < num_samples; ++i)
    output[i] = osc.next() * env.next();
```

| Method | Description |
|---|---|
| `set_params(Adsr::Params)` | Set ADSR times and sustain level. |
| `set_sample_rate(float)` | Set sample rate (required). |
| `note_on()` | Trigger attack stage. |
| `note_off()` | Trigger release stage. |
| `reset()` | Reset to idle, level = 0. |
| `next()` | Return next envelope sample (0-1). |
| `is_active()` | True if not idle. |
| `stage()` | Current stage: `idle`, `attack`, `decay`, `sustain`, `release`. |

**Params struct:**

| Field | Default | Unit | Range |
|---|---|---|---|
| `attack` | 0.01 | seconds | > 0 |
| `decay` | 0.1 | seconds | > 0 |
| `sustain` | 0.7 | level | 0-1 |
| `release` | 0.3 | seconds | > 0 |

Setting attack/decay/release to 0 causes instant transitions (rate = 1.0).

**Sample rate dependency:** `set_sample_rate()` must be called before use.

---

## 3. Oscillators

### Oscillator

Band-limited oscillator with polyBLEP anti-aliasing. Supports sine, saw, square, and triangle waveforms.

```cpp
signal::Oscillator osc;
osc.set_sample_rate(sample_rate);
osc.set_waveform(signal::Oscillator::Waveform::saw);
osc.set_frequency(440.0f);

for (int i = 0; i < num_samples; ++i)
    output[i] = osc.next();
```

| Method | Description |
|---|---|
| `set_sample_rate(float)` | Set sample rate. |
| `set_frequency(float hz)` | Set oscillator frequency. |
| `set_waveform(Waveform)` | Set waveform type. |
| `reset()` | Reset phase to 0. |
| `next()` | Generate next sample. |
| `phase()` | Current phase (0-1). |
| `frequency()` | Current frequency. |

**Waveforms:** `sine`, `saw`, `square`, `triangle`.

PolyBLEP anti-aliasing reduces aliasing artifacts on saw, square, and triangle waveforms. Triangle is generated via leaky integration of a polyBLEP square wave.

**Sample rate dependency:** `set_sample_rate()` must be called. Output range is approximately [-1, 1].

**Caveat:** The triangle waveform uses a leaky integrator, which needs a few cycles to stabilize after `reset()` or frequency changes.

---

## 4. Filters

### Biquad

Standard second-order IIR filter (biquad). Supports 8 filter types. Uses Direct Form II Transposed implementation.

```cpp
signal::Biquad lpf;
lpf.set_coefficients(signal::Biquad::Type::lowpass, 2000.0f, 0.707f, sample_rate);

// Per-sample:
float out = lpf.process(input_sample);

// Per-buffer:
lpf.process(buffer, num_samples);  // in-place
```

| Method | Description |
|---|---|
| `set_coefficients(type, freq_hz, q, sample_rate, gain_db)` | Configure filter. `gain_db` only used for peaking/shelf types. |
| `process(float)` | Filter one sample, returns output. |
| `process(float*, int)` | Filter buffer in-place. |
| `reset()` | Clear internal state (call on discontinuities). |

**Filter types:**

| Type | Use | `gain_db` used? |
|---|---|---|
| `lowpass` | Low-pass filter | No |
| `highpass` | High-pass filter | No |
| `bandpass` | Band-pass filter | No |
| `notch` | Notch (band-reject) | No |
| `allpass` | All-pass filter | No |
| `peaking` | Parametric EQ band | Yes |
| `low_shelf` | Low shelf EQ | Yes |
| `high_shelf` | High shelf EQ | Yes |

**Parameters:**
- `freq_hz`: Center/cutoff frequency in Hz
- `q`: Quality factor (0.707 = Butterworth, higher = more resonant)
- `gain_db`: Boost/cut in dB (peaking and shelf types only)

**Sample rate dependency:** `sample_rate` is a parameter of `set_coefficients()`. Call again if sample rate changes.

---

### Svf

State Variable Filter using Topology Preserving Transform (TPT). Numerically stable at all frequencies with no Nyquist cramping. Provides simultaneous lowpass, highpass, bandpass, and notch outputs (one selected via mode).

```cpp
signal::Svf filter;
filter.set_sample_rate(sample_rate);
filter.set_frequency(1000.0f);
filter.set_resonance(2.0f);
filter.set_mode(signal::Svf::Mode::lowpass);

for (int i = 0; i < num_samples; ++i)
    output[i] = filter.process(input[i]);
```

| Method | Description |
|---|---|
| `set_sample_rate(float)` | Set sample rate. Recalculates coefficients. |
| `set_frequency(float hz)` | Set cutoff frequency. Recalculates coefficients. |
| `set_resonance(float q)` | Set Q factor (default 0.707). Recalculates coefficients. |
| `set_mode(Mode)` | Select output: `lowpass`, `highpass`, `bandpass`, `notch`. |
| `process(float)` | Filter one sample. |
| `process(float*, int)` | Filter buffer in-place. |
| `reset()` | Clear internal state. |

**Sample rate dependency:** `set_sample_rate()` required. Coefficients update automatically when frequency, resonance, or sample rate change.

---

### LadderFilter

Moog-style 4-pole (24 dB/oct) ladder filter with nonlinear feedback. Self-oscillates at high resonance.

```cpp
signal::LadderFilter ladder;
ladder.set_sample_rate(sample_rate);
ladder.set_frequency(800.0f);
ladder.set_resonance(0.8f);  // 0-1, high values self-oscillate

for (int i = 0; i < num_samples; ++i)
    output[i] = ladder.process(input[i]);
```

| Method | Description |
|---|---|
| `set_sample_rate(float)` | Set sample rate. |
| `set_frequency(float hz)` | Set cutoff frequency. |
| `set_resonance(float)` | Set resonance. Clamped to [0, 1]. Values near 1 self-oscillate. |
| `process(float)` | Filter one sample. |
| `process(float*, int)` | Filter buffer in-place. |
| `reset()` | Clear all stage states. |

Uses `tanh()` saturation per stage for analog-style nonlinearity. The feedback path creates resonance: `resonance * 4.0 * (stage[3] - input * 0.5)`.

**Sample rate dependency:** `set_sample_rate()` required.

**Caveat:** High resonance with hot input signals can produce loud self-oscillation. Consider a limiter on the output.

---

### LinkwitzRiley

4th-order Linkwitz-Riley crossover filter (-6 dB at crossover frequency). Provides lowpass and highpass outputs that sum flat. Built from two cascaded Biquad lowpass and two cascaded Biquad highpass filters.

```cpp
signal::LinkwitzRiley xover;
xover.set_frequency(2000.0f, sample_rate);

for (int i = 0; i < num_samples; ++i) {
    auto [low, high] = xover.process(input[i]);
    low_band[i] = low;
    high_band[i] = high;
}
```

| Method | Description |
|---|---|
| `set_frequency(float hz, float sample_rate)` | Set crossover frequency. |
| `process(float)` | Split one sample, returns `BandSplit{low, high}`. |
| `reset()` | Clear all filter states. |

**Sample rate dependency:** `sample_rate` is a parameter of `set_frequency()`.

---

## 5. Delays

### DelayLine

Variable-length delay line with linear interpolation for fractional delays. Must call `prepare()` before use (allocates the internal buffer).

```cpp
signal::DelayLine delay;

// In prepare():
int max_delay = static_cast<int>(sample_rate * 0.5f);  // 500ms max
delay.prepare(max_delay);

// In process():
delay.push(input_sample);
float delayed = delay.read(delay_in_samples);

// Or combined:
float out = delay.process(input_sample, delay_in_samples);
```

| Method | Description |
|---|---|
| `prepare(int max_delay_samples)` | Allocate buffer. **Not real-time safe.** |
| `push(float)` | Write a sample into the delay line. |
| `read(float delay_samples)` | Read at fractional delay with linear interpolation. |
| `read(int delay_samples)` | Read at integer delay. |
| `process(float input, float delay_samples)` | Push and read in one call. |
| `reset()` | Zero the buffer. |
| `max_delay()` | Maximum delay in samples. |

**Sample rate dependency:** Convert time to samples manually (`seconds * sample_rate`).

**Caveat:** `prepare()` allocates memory. Call it in `Processor::prepare()`, never on the audio thread.

---

### Chorus

Stereo chorus using modulated delay lines. Produces stereo output from mono input with 90-degree LFO phase offset between channels.

```cpp
signal::Chorus chorus;

// In prepare():
chorus.prepare(sample_rate);

// In process():
chorus.set_rate(1.5f);       // LFO rate in Hz
chorus.set_depth(0.5f);      // modulation depth 0-1
chorus.set_mix(0.5f);        // dry/wet 0-1
chorus.set_delay_ms(15.0f);  // center delay

auto [left, right] = chorus.process(mono_input);
```

| Method | Description |
|---|---|
| `prepare(float sample_rate)` | Allocate delay buffers (50ms max). **Not real-time safe.** |
| `set_rate(float hz)` | LFO rate. Typical range: 0.1-5 Hz. |
| `set_depth(float)` | Modulation depth [0, 1]. |
| `set_mix(float)` | Dry/wet mix [0, 1]. |
| `set_delay_ms(float)` | Center delay time. Typical range: 5-30 ms. |
| `process(float)` | Process mono input, returns `StereoSample{left, right}`. |
| `reset()` | Clear delay buffers and reset LFO phase. |

**Sample rate dependency:** `prepare()` must be called with the current sample rate.

---

### Phaser

Phaser effect using cascaded first-order allpass filters with LFO modulation and feedback.

```cpp
signal::Phaser phaser;
phaser.set_sample_rate(sample_rate);
phaser.set_rate(0.5f);        // LFO Hz
phaser.set_depth(0.7f);       // 0-1
phaser.set_feedback(0.5f);    // -0.95 to 0.95
phaser.set_stages(4);         // 2-8 stages
phaser.set_mix(0.5f);

// Per-sample:
float out = phaser.process(input_sample);

// Per-buffer:
phaser.process(buffer, num_samples);
```

| Method | Description |
|---|---|
| `set_sample_rate(float)` | Set sample rate. |
| `set_rate(float hz)` | LFO rate. |
| `set_depth(float)` | Modulation depth [0, 1]. Maps LFO to 200-5000 Hz range. |
| `set_feedback(float)` | Feedback amount. Clamped to [-0.95, 0.95]. |
| `set_mix(float)` | Dry/wet mix [0, 1]. |
| `set_stages(int)` | Number of allpass stages. Clamped to [2, 8]. |
| `process(float)` | Process one sample. |
| `process(float*, int)` | Process buffer in-place. |
| `reset()` | Clear all state. |

**Sample rate dependency:** `set_sample_rate()` required.

---

## 6. Dynamics

### Compressor

Feed-forward compressor with soft knee, adjustable attack/release, and makeup gain.

```cpp
signal::Compressor comp;
comp.set_sample_rate(sample_rate);
comp.set_params({
    .threshold_db = -20.0f,
    .ratio        = 4.0f,
    .attack_ms    = 5.0f,
    .release_ms   = 100.0f,
    .knee_db      = 6.0f,
    .makeup_db    = 0.0f,
});

for (int i = 0; i < num_samples; ++i)
    output[i] = comp.process(input[i]);

float gr = comp.gain_reduction_db();  // for metering
```

| Method | Description |
|---|---|
| `set_params(Compressor::Params)` | Set all compressor parameters. |
| `set_sample_rate(float)` | Set sample rate. |
| `process(float)` | Compress one sample. |
| `process(float*, int)` | Compress buffer in-place. |
| `gain_reduction_db()` | Current gain reduction (for meters). |
| `reset()` | Reset envelope to 0. |

**Params struct:**

| Field | Default | Unit | Description |
|---|---|---|---|
| `threshold_db` | -20.0 | dB | Level where compression begins |
| `ratio` | 4.0 | :1 | Compression ratio |
| `attack_ms` | 5.0 | ms | Attack time |
| `release_ms` | 100.0 | ms | Release time |
| `knee_db` | 6.0 | dB | Soft knee width (0 = hard knee) |
| `makeup_db` | 0.0 | dB | Output makeup gain |

**Sample rate dependency:** `set_sample_rate()` required (used for attack/release coefficients).

---

### Limiter

Brickwall limiter with instant attack and configurable release. Lookahead-free design.

```cpp
signal::Limiter limiter;
limiter.set_sample_rate(sample_rate);
limiter.set_threshold_db(-1.0f);
limiter.set_release_ms(50.0f);

for (int i = 0; i < num_samples; ++i)
    output[i] = limiter.process(input[i]);
```

| Method | Description |
|---|---|
| `set_threshold_db(float)` | Set ceiling in dB. Converted to linear internally. |
| `set_release_ms(float)` | Release time in ms. |
| `set_sample_rate(float)` | Set sample rate. |
| `process(float)` | Limit one sample. |
| `process(float*, int)` | Limit buffer in-place. |
| `reset()` | Reset envelope. |

Default threshold: 0 dB (unity). Default release: 50 ms.

**Sample rate dependency:** `set_sample_rate()` required.

---

### NoiseGate

Noise gate / downward expander. Attenuates signal below threshold with configurable expansion ratio and range limit.

```cpp
signal::NoiseGate gate;
gate.set_sample_rate(sample_rate);
gate.set_params({
    .threshold_db = -40.0f,
    .ratio        = 10.0f,   // 10:1 expansion
    .attack_ms    = 0.5f,
    .release_ms   = 50.0f,
    .range_db     = -80.0f,  // max attenuation
});

for (int i = 0; i < num_samples; ++i)
    output[i] = gate.process(input[i]);
```

| Method | Description |
|---|---|
| `set_params(NoiseGate::Params)` | Set gate parameters. |
| `set_sample_rate(float)` | Set sample rate. |
| `process(float)` | Gate one sample. |
| `process(float*, int)` | Gate buffer in-place. |
| `reset()` | Reset envelope. |

**Params struct:**

| Field | Default | Unit | Description |
|---|---|---|---|
| `threshold_db` | -40.0 | dB | Gate opens above this level |
| `ratio` | 10.0 | :1 | Expansion ratio (higher = harder gate) |
| `attack_ms` | 0.5 | ms | How fast the gate closes |
| `release_ms` | 50.0 | ms | How fast the gate opens |
| `range_db` | -80.0 | dB | Maximum attenuation floor |

**Sample rate dependency:** `set_sample_rate()` required.

---

## 7. Effects

### Reverb

Feedback Delay Network (FDN) reverb with 4 delay lines, Hadamard mixing matrix, and one-pole damping filters. Produces stereo output from mono input.

```cpp
signal::Reverb reverb;

// In prepare():
reverb.prepare(sample_rate);

// In process():
reverb.set_decay(2.0f);    // RT60 in seconds
reverb.set_damping(0.3f);  // high-frequency damping 0-0.99
reverb.set_mix(0.3f);      // dry/wet 0-1

auto [left, right] = reverb.process(mono_input);
```

| Method | Description |
|---|---|
| `prepare(float sample_rate)` | Allocate delay buffers. **Not real-time safe.** |
| `set_decay(float seconds)` | Reverb tail time (RT60). |
| `set_damping(float)` | High-frequency damping [0, 0.99]. Higher = darker. |
| `set_mix(float)` | Dry/wet mix [0, 1]. |
| `process(float)` | Process mono input, returns `StereoSample{left, right}`. |
| `reset()` | Clear all delay lines and filter states. |

Uses prime-number delay lengths (1087, 1283, 1481, 1693 samples at 44.1 kHz, scaled for other rates) for maximum echo density. Feedback gain is derived from RT60: `10^(-3 * avg_delay / decay)`.

**Sample rate dependency:** `prepare()` must be called with the current sample rate. Delay lengths scale proportionally.

---

### WaveShaper

Waveshaping distortion with five curve types.

```cpp
signal::WaveShaper shaper;
shaper.set_curve(signal::WaveShaper::Curve::tanh_clip);
shaper.set_drive(3.0f);

// Per-sample:
float out = shaper.process(input_sample);

// Per-buffer:
shaper.process(buffer, num_samples);
```

| Method | Description |
|---|---|
| `set_curve(Curve)` | Select waveshaping function. |
| `set_drive(float)` | Input gain multiplier before shaping. |
| `process(float)` | Shape one sample. |
| `process(float*, int)` | Shape buffer in-place. |

**Curves:**

| Curve | Formula | Character |
|---|---|---|
| `soft_clip` | `x / (1 + |x|)` | Gentle saturation |
| `hard_clip` | `clamp(x, -1, 1)` | Digital clipping |
| `tanh_clip` | `tanh(x)` | Smooth analog-style saturation |
| `fold` | Fold-back at +/- 1 | Wave folding |
| `sine_fold` | `sin(x * pi/2)` | Sinusoidal wave folding |

Default: `tanh_clip`, drive = 1.0.

**Sample rate dependency:** None. Consider using `Oversampler` to reduce aliasing from nonlinear shaping.

---

### Oversampler

Runs a processing callback at 2x or 4x sample rate with anti-aliasing filters on input and output.

```cpp
signal::Oversampler os;
os.set_factor(signal::Oversampler::Factor::x2);
os.set_sample_rate(sample_rate);

signal::WaveShaper shaper;
shaper.set_drive(4.0f);

for (int i = 0; i < num_samples; ++i) {
    output[i] = os.process(input[i], [&](float s) {
        return shaper.process(s);
    });
}
```

| Method | Description |
|---|---|
| `set_factor(Factor)` | `Factor::x2` or `Factor::x4`. |
| `set_sample_rate(float)` | Set base sample rate. Configures anti-aliasing filters. |
| `process(float, callback)` | Upsample, process via callback, downsample. |
| `reset()` | Clear filter states. |

The anti-aliasing filters are lowpass Biquads set to `0.4 * base_sample_rate` at the oversampled rate. Energy is preserved by scaling the upsampled signal by the oversampling factor.

**Sample rate dependency:** `set_sample_rate()` required.

**Caveat:** The callback uses `std::function<float(float)>`, which may allocate on first use. In latency-critical paths, consider pre-allocating or using a lambda that captures by reference.

---

## 8. Analysis

### Fft

Radix-2 in-place FFT (decimation-in-time). Size must be a power of 2. Pre-computes twiddle factors at construction.

```cpp
signal::Fft fft(1024);

std::vector<std::complex<float>> freq(1024);
fft.forward_real(audio_buffer, freq.data());

std::vector<float> magnitudes(512);
fft.magnitude_db(freq.data(), magnitudes.data(), 512);
```

| Method | Description |
|---|---|
| `Fft(int size)` | Construct with FFT size (must be power of 2). |
| `size()` | FFT size. |
| `forward(complex<float>*)` | In-place forward FFT. |
| `inverse(complex<float>*)` | In-place inverse FFT (with 1/N scaling). |
| `forward_real(float* in, complex<float>* out)` | Real input to complex frequency domain. |
| `magnitude_db(complex<float>*, float*, int)` | Compute magnitude spectrum in dB. |
| `magnitude(complex<float>*, float*, int)` | Compute magnitude spectrum (linear). |

**Sample rate dependency:** None (operates in sample domain). To convert bin index to frequency: `bin * sample_rate / fft_size`.

**Caveat:** The constructor allocates twiddle factor storage. Create `Fft` objects in `prepare()`, not on the audio thread. `forward()` and `inverse()` are real-time safe.

---

### Convolver

Frequency-domain convolver using overlap-add. Loads an impulse response and convolves input signal in blocks.

```cpp
signal::Convolver conv;

// In prepare() — load IR (allocates):
conv.load_ir(ir_data, ir_length, 256);  // block_size = 256

// In process():
for (int i = 0; i < num_samples; ++i)
    output[i] = conv.process(input[i]);

// Or buffer-based:
conv.process(input_buf, output_buf, num_samples);
```

| Method | Description |
|---|---|
| `load_ir(float* ir, int length, int block_size)` | Load impulse response. Block size defaults to 256 if 0. **Not real-time safe.** |
| `process(float)` | Convolve one sample. Returns output. |
| `process(float* in, float* out, int n)` | Convolve a buffer. |
| `reset()` | Clear all internal buffers. |

The FFT size is automatically chosen as the next power of 2 >= `block_size + ir_length`. Processing happens in blocks: samples accumulate until `block_size` is reached, then a full FFT-multiply-IFFT cycle runs.

**Sample rate dependency:** The IR itself is sample-rate dependent. If you change sample rate, reload the IR at the new rate.

**Caveat:** `load_ir()` allocates multiple buffers. Call only in `prepare()`. The per-sample `process()` is real-time safe (no allocations).

---

### WindowFunction

Generates window functions for FFT analysis, spectral processing, and FIR filter design.

```cpp
auto window = signal::WindowFunction::generate(1024, signal::WindowFunction::Type::hann);

// Apply to a buffer before FFT:
signal::WindowFunction::apply(audio_buffer, window);
fft.forward_real(audio_buffer, freq.data());
```

| Method | Description |
|---|---|
| `generate(int size, Type, float param)` | Generate a window. Returns `std::vector<float>`. `param` used only for Kaiser. |
| `apply(float* buffer, vector<float>& window)` | Multiply buffer by window in-place. |

**Window types:**

| Type | Use case |
|---|---|
| `rectangular` | No windowing (unity) |
| `hann` | General-purpose spectral analysis |
| `hamming` | FIR filter design |
| `blackman` | High dynamic range spectral analysis |
| `flat_top` | Amplitude-accurate measurements |
| `kaiser` | Adjustable main-lobe/side-lobe tradeoff (set `param` = beta, default 3.0) |

**Sample rate dependency:** None.

**Caveat:** `generate()` allocates a vector. Call during `prepare()`, not on the audio thread.
