# Character Delay

`pulp::signal::CharacterDelay` is a stereo, wet-only delay engine with five
feedback-loop characters: clean, vintage digital, tape, BBD, and diffusion.
The delay frame is shared; the selected character changes what each repeat
passes through before it re-enters the loop. Color therefore accumulates from
repeat to repeat instead of being applied once after the delay.

```cpp
#include <pulp/signal/character_delay.hpp>

pulp::signal::CharacterDelay delay;

// Processor::prepare(), with processing stopped.
delay.set_sample_rate(sample_rate);
delay.set_character(pulp::signal::CharacterDelay::Character::tape);
delay.set_tape_tier(pulp::signal::CharacterDelay::TapeTier::standard);

// On the audio thread, before process().
delay.set_time_ms(375.0f);
delay.set_time_offset(1.01f);
delay.set_feedback(0.55f);
delay.set_crossfeed(0.25f);
delay.set_character_amount(0.7f);
delay.set_mod(0.3f, 0.15f);
delay.set_duck(0.2f);

// left and right are replaced in place with WET-ONLY output.
delay.process(left, right, num_samples);
```

Keep a copy of the dry input, or use a graph dry/wet node, when the effect needs
a dry signal. `CharacterDelay` deliberately has no mix parameter.

## Lifecycle and thread contract

`CharacterDelay` is lock-free, allocation-free during `process()` and
`reset()`, and not safe for concurrent mutation. Use it in this order:

1. Call `set_sample_rate()` before the first render. It allocates all delay,
   reverse, character, and physical-tape storage.
2. With processing stopped, select the character and tape configuration.
3. On the same audio thread that calls `process()`, publish current parameter
   values through the realtime parameter setters.
4. Call `process()` in place on stereo buffers.
5. Call `reset()` between renders or blocks when state must be cleared; never
   race it against `process()`.

If controls arrive on another thread, move them through the host parameter
store or another realtime-safe publication mechanism. Do not call setters on
one thread while `process()` reads the object on another.

The engine reports zero host latency in every mode. The physical tape tier has
internal group delay, but that delay is inside the feedback loop and is folded
out of the requested delay-line distance.

## SignalGraph and generated patches

The host catalog exposes six registration-time realizations through
`pulp::host::character_delay::make_character_delay_node()`:
`delay.clean`, `delay.vintage`, `delay.tape`, `delay.tape_physical`,
`delay.bbd`, and `delay.diffusion`.

Character, tape tier, and physical-tape speed are construction-time choices,
not injectable parameters. The character changes buffer topology and active
stages, so a baked graph cannot automate one realization into another. Importers
and generators should select the stable type id they need, then automate the ten
baked parameters: time, time offset, feedback, crossfeed, character amount,
modulation rate, modulation depth, duck, freeze, and reverse. Catalog nodes are
also stereo and wet-only; compose them with `make_drywet_node()` instead of
instancing two mono delays or assuming a built-in mix.

## Choosing a character

| Character | Feedback-loop behavior | Good starting point |
|---|---|---|
| `clean` | Full-bandwidth fractional delay with loop high/low-pass shaping | Precise echoes, null-sensitive work, modern digital delay |
| `vintage_digital` | Reduced-rate converter loop, emphasis, quantization, and dither | Early rack-delay grit and pitch-gliding time changes |
| `tape` | Wow, flutter, saturation, head bump, and progressive loss | Dub echoes and unstable repeats; select `physical` for hysteresis and physics-derived loss |
| `bbd` | Clock-dependent bandwidth, companding, noise, and clock artifacts | Dark analog repeats whose bandwidth follows delay time |
| `diffusion` | Modulated allpass diffusion inside the loop | Echoes that smear toward a soft reverb cloud |

`set_character_amount(0)` is the least colored calibration point for the
selected character; `1` is the most colored. It does not crossfade between the
five character types.

## Complete API reference

The implementation sources of truth are
`core/signal/include/pulp/signal/character_delay.hpp` for the API and
`core/signal/include/pulp/signal/character_delay/tables.hpp` for calibration
constants. This guide restates that contract for users; update it with either
source so ranges and defaults do not drift.

The default alias is `CharacterDelayT<float>`:

```cpp
using CharacterDelay = CharacterDelayT<float>;
```

`CharacterDelayT<double>` accepts and returns double-precision samples, but it
is not an end-to-end double implementation: delay, reverse, and BBD storage are
float-backed. Use it to avoid interface conversions in a surrounding double
render path, not to claim a wholly double-precision signal path. All public
parameter setters preserve their previous value when given a non-finite
floating-point input.

### Defaults

| Setting | Default |
|---|---|
| Character / tape tier | `clean` / `standard` |
| Sample rate | 48,000 Hz; call `set_sample_rate()` before rendering even when this matches the host |
| Left time / right offset | 350 ms / `1.0` |
| Feedback / crossfeed | `0.35` / `0.0` |
| Character amount / duck | `0.5` / `0.0` |
| Modulation rate / depth | 0.5 Hz / `0.0` |
| Tape speed | 7.5 ips |
| Freeze / reverse | off / off |

### Types

| Type | Values | Meaning |
|---|---|---|
| `Character` | `clean`, `vintage_digital`, `tape`, `bbd`, `diffusion` | Selects the processor placed inside the feedback loop. |
| `TapeTier` | `standard`, `physical` | Selects the tape implementation. `physical` adds 8x-oversampled hysteresis, physics-derived loss, and wear artifacts. |

### Configuration methods

These methods belong to the stopped-processing/configuration phase.

| Method | Behavior |
|---|---|
| `set_sample_rate(double sample_rate)` | Allocates storage for a 2,000 ms left delay plus the maximum right-channel offset, prepares every character, reseeds deterministic noise sources, and resets the engine. Finite rates are clamped to at least 1,000 Hz; a non-finite rate is ignored and does not reset state. Must be called before `process()`. |
| `set_character(Character character)` | Selects the active character and reconfigures that character's delay-time slew. Switch only between renders or while processing is stopped. |
| `set_tape_tier(TapeTier tier)` | Selects standard or physical tape processing. It affects only the tape character. Switch while processing is stopped. |
| `set_tape_speed_ips(SampleType ips)` | Sets physical-tape speed continuously from 1.875 to 30 inches per second, clamping finite values to that range. The calibrated landmarks are 1.875, 3.75, 7.5, 15, and 30 ips. It has no audible effect in the standard tape tier. |

### Realtime parameter methods

Call these on the same audio thread as `process()`. Normalized inputs are
clamped. Delay time, feedback, crossfeed, character amount, ducking, and freeze
use internal smoothing; modulation controls update directly.

| Method | Range and behavior |
|---|---|
| `set_time_ms(SampleType left_ms)` | Sets left-channel delay time in milliseconds, clamped to `[1, 2000]`. Time changes slew continuously and produce pitch motion rather than a crossfade jump. |
| `set_time_offset(SampleType multiplier)` | Sets right time as a multiple of left time, clamped to `[0.5, 1.5]`. The largest addressable right delay is therefore 3,000 ms. |
| `set_feedback(SampleType feedback)` | Sets requested feedback, clamped to `[0, 1.1]`. Tape, BBD, and vintage digital use in-loop saturation and retain the over-unity range for self-oscillation. Clean and diffusion are bounded to `0.98` internally. Physical tape applies age-dependent stability compensation. |
| `set_crossfeed(SampleType crossfeed)` | Sets stereo crossfeed in `[0, 1]`; `0` keeps channels independent and `1` exchanges their feedback taps. Reverse mode smoothly forces effective crossfeed to zero because its channel segments are independent. |
| `set_character_amount(SampleType amount)` | Sets the selected character's intensity in `[0, 1]`. The parameter is smoothed and updates character coefficients at control rate. |
| `set_mod(SampleType rate01, SampleType depth01)` | Sets two independent normalized controls. `rate01` maps exponentially from 0.05 to 10 Hz. `depth01` maps linearly to at most 5% delay-time modulation. If one argument is non-finite, only that argument is ignored. |
| `set_duck(SampleType amount01)` | Sets wet-output ducking in `[0, 1]`. The detector reads dry input before the feedback loop, uses a 5 ms attack and 250 ms release, and lets repeats bloom between source phrases. |
| `set_freeze(bool on)` | Smoothly suppresses new input and forces the feedback scalar to unity, overriding the ordinary per-character feedback ceiling. This is unity recirculation, not a promise of bit-exact hold: tape, BBD, vintage-digital, and diffusion stages continue evolving the material inside the loop. |
| `set_reverse(bool on)` | Enables independently segmented reverse playback for each channel. The forward line continues recording, so disabling reverse resumes current material rather than stale buffered audio. |

### Runtime methods

| Method | Behavior |
|---|---|
| `latency_samples() const noexcept` | Always returns `0`. Internal physical-tape delay is compensated inside the requested echo time rather than reported to the host. |
| `reset() noexcept` | Allocation-free. Clears delay, reverse, filter, solver, modulation, and detector state; reseeds noise sources; and snaps smoothing state to current targets. Repeated renders after reset are deterministic. Call on the audio thread between blocks or while stopped, never concurrently with `process()`. |
| `process(SampleType* left, SampleType* right, int num_samples) noexcept` | Processes stereo buffers in place and writes wet-only output. It is allocation-free after preparation. A null channel pointer is a no-op. Non-positive sample counts process no samples. |

### Diagnostic and validation methods

These public hooks expose calibration state used by Pulp's conformance tests.
They are useful for offline diagnostics, but they are not ordinary plugin
parameters. Treat them as stopped-processing or single-thread inspection APIs;
do not read or mutate them concurrently with `process()`.

| Method | Behavior |
|---|---|
| `set_bbd_compander_enabled(bool enabled) noexcept` | Enables or bypasses the BBD compander on both channels for controlled A/B validation. |
| `bbd_bandwidth_hz() const noexcept` | Returns the current left-channel BBD bandwidth calibration in hertz. |
| `bbd_stages() const noexcept` | Returns the current left-channel modeled BBD stage count. |
| `vintage_band_edge_hz() const noexcept` | Returns the current left-channel vintage-digital anti-alias band edge in hertz. |
| `vintage_internal_rate_hz() const noexcept` | Returns the current left-channel vintage-digital internal sample rate in hertz. |
| `chew_state_index(int channel) const noexcept` | Returns the physical-tape chew state index for channel `0` or `1`. The caller must provide a valid channel index. |
| `tape_gap_coefficients(int channel) const noexcept` | Returns a reference to the physical-tape gap-loss FIR coefficients for channel `0` or `1`. The reference remains owned by the delay instance and can be invalidated by `set_sample_rate()` or `set_tape_speed_ips()`; the caller must provide a valid channel index. |

## Recipes

### Tempo-synced time

The DSP accepts milliseconds so transport policy stays outside the signal
module. Convert the host tempo and the desired beat length before setting time:

```cpp
double milliseconds_for_beats(double bpm, double beats) {
    return 60000.0 * beats / bpm;
}

delay.set_time_ms(static_cast<float>(milliseconds_for_beats(bpm, 0.75)));
// 0.75 beat = dotted eighth when one beat is a quarter note.
```

Clamp or validate host tempo before division. Useful quarter-note beat lengths
include `1.0` (quarter), `0.5` (eighth), `0.75` (dotted eighth), and
`1.0 / 3.0` (eighth-note triplet).

### Ping-pong without losing stereo width

Start with a small channel-time offset and moderate crossfeed:

```cpp
delay.set_time_offset(1.01f);
delay.set_crossfeed(0.65f);
```

The offset decorrelates otherwise identical repeats. Reverse mode intentionally
removes crossfeed, so use forward playback for a conventional ping-pong pattern.

### Dub hold and release

Tape, BBD, and vintage digital can accept feedback above unity because their
loop saturators bound energy. Raise `set_feedback()` gradually toward or above
`1.0`, then use `set_freeze(true)` to reject new input and recirculate with a
unity feedback scalar. Colored modes intentionally keep evolving while frozen;
freeze does not bypass their in-loop loss, noise, diffusion, or saturation.
Keep the output gain under host control: self-oscillation is intentionally
capable of sustained high level.

### Physical tape cost and minimum time

The physical tape tier is the quality/cost lane. Its hysteresis stage runs at
8x and its loss model adds an in-loop FIR/IIR cascade. The engine compensates
that group delay within the requested echo time, but extremely short requested
times clamp at the tier's realizable internal floor. Use the standard tier when
CPU cost or sub-millisecond loop behavior matters more than magnetic detail.

## Common mistakes

- Mixing the processed buffers as though they still contain dry signal.
- Calling `set_sample_rate()`, switching characters, or changing tape tier from
  a control thread while audio is running.
- Reporting extra host latency for the physical tape tier even though
  `latency_samples()` already returns the complete contract.
- Expecting `set_character_amount()` to morph between character enum values.
- Treating diagnostic hooks as stable user parameters or passing a channel
  index other than `0` or `1`.
