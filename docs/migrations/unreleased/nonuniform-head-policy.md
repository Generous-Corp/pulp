---
breaking = false
summary = "NonUniformPartitionedConvolver picks its head length from a measured table when no tail multiplier is given (Apple Silicon, float): up to 14x faster for long IRs, output unchanged to float rounding"
---

## What changed

- `NonUniformPartitionedConvolverT::load_ir(ir, length, block)` (no tail
  multiplier) now uses the new `default_tail_multiplier(length, block)`.
  - On Apple Silicon macOS with the Accelerate backend in single precision, on
    the chip families the table was measured on (Apple M1 and M5, read from
    `machdep.cpu.brand_string`), it picks the head length (K·B samples) from
    the measured table `choose_tail_multiplier()` for blocks of 16 to 512
    frames.
  - Everywhere else it keeps the previous fixed K of 4
    (`kDefaultTailMultiplier`): other chip families, other platforms and FFT
    backends, double precision, and blocks outside that range.
- For long IRs the fixed K of 4 cost 1.5-14x the best measured head length.
  Examples: 96000 taps at 32-frame blocks, and 48000 taps at 16 frames.
- The four-argument `load_ir()` with an explicit tail multiplier is unchanged.

## Effect on output

- The output differs from before only by float rounding: at most 4.3e-6 of
  the output RMS against the uniform engine over the whole receipt matrix.
- `head_samples()`, `tail_block()` and `tail_multiplier()` report the chosen
  values.

## Migration steps

- To keep the previous partitioning exactly, pass
  `NonUniformPartitionedConvolver::kDefaultTailMultiplier` as the fourth
  argument.
