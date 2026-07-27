# Dynamics Processors

Pulp provides four complementary compressor designs. They share an explicit
`prepare()`/`reset()` lifecycle, expose their gain computer for meters and curve
editors, and report latency without requiring an audio probe. Use the float
aliases shown below; every class also has a `64` double-precision alias.

| Processor | Header | Choose it for |
|---|---|---|
| `FeedforwardCompressor` | `<pulp/signal/feedforward_compressor.hpp>` | Transparent peak/RMS compression, stereo linking, lookahead, and auto makeup |
| `VcaCompressor` | `<pulp/signal/vca_compressor.hpp>` | Program-dependent RMS behavior, OverEasy-style knee, and infinity-plus curves |
| `DiodeBridgeCompressor` | `<pulp/signal/diode_bridge_compressor.hpp>` | Feedback dynamics with diode-bridge and transformer character |
| `FetCompressor` | `<pulp/signal/fet_compressor.hpp>` | Fixed-threshold input-drive workflow, ratio buttons, fast attack, and transformer output |

## Minimal insert

```cpp
#include <pulp/signal/feedforward_compressor.hpp>

pulp::signal::FeedforwardCompressor compressor;

// Processor::prepare(), never the audio callback.
compressor.prepare(sample_rate, 10.0);
compressor.set_threshold_db(-18.0);
compressor.set_ratio(4.0);
compressor.set_attack_ms(8.0);
compressor.set_release_ms(120.0);
compressor.set_lookahead_ms(2.0);

// Processor::process().
compressor.process_block_stereo(left, right, num_frames);
const int latency = compressor.latency_samples(); // Return from your Processor.
```

`prepare()` sizes lookahead or oversampling state. Keep it off the audio thread.
The setters and processing calls are allocation-free after preparation. Call
`reset()` on transport discontinuities; do not call `prepare()` merely to clear
history.

## Choosing a topology

- Start with `FeedforwardCompressor` when the requirement is level control rather
  than audible character. Select peak or RMS detection explicitly and use
  `static_curve_db()` to draw the same curve the processor applies.
- Use `VcaCompressor` when one time control and an attack/release ratio are part
  of the interaction model. `gain_computer_unclamped_db()` exposes where the
  ceiling floor begins to affect the curve.
- Use `DiodeBridgeCompressor` when the feedback topology is intentional. The
  `character` macro drives both bridge and transformer color; `set_adaa()` is a
  measurement switch, not an ordinary user control.
- Use `FetCompressor` for the fixed-threshold, input-drive convention. The
  nominal ratio and measured closed-loop ratio are deliberately separate
  quantities; expose the nominal ratio to users and the measured accessors to
  analysis or visualization code.

All gain-reduction meters are valid after processing a sample. Sign conventions
differ where the circuit convention differs: consult the
[complete advanced DSP API](../reference/advanced-dsp-api.md#dynamics) before
combining meter values across processor types.

## Verification

Test the memoryless curve separately from detector timing, then test the full
processor over the sample-rate/block-size matrix. Lookahead must be reflected in
the owning `Processor::latency_samples()`. For audible comparison, render both
paths offline and use the audio quality lab; do not open a live device merely to
check a curve or envelope.
