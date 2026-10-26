---
breaking = true
summary = "PartitionedConvolver keeps its IR spectra and input history in ConvolverSpectrumArenaT (one compact block of half spectra) instead of std::vector<std::vector<std::complex>>; output unchanged bit for bit"
---

## What changed

`ConvolverIrStateT::ir_spectra` and `ConvolverInputHistoryT::spectra` are now
`ConvolverSpectrumArenaT<SampleType>`, not
`std::vector<std::vector<std::complex<SampleType>>>`.

- Each set is a single 64-byte-aligned allocation. Every spectrum holds only the
  `fft_size / 2 + 1` bins (DC..Nyquist) that the multiply-accumulate and the
  real inverse transform read; previously each was a separate vector of
  `fft_size` bins.
- `operator[](i)` returns a `std::span` of `bins()` values, so `[i].data()`,
  `[i].size()`, `[i].begin()` and `[i].end()` keep working.
- `size()` and `empty()` count spectra, as before. `bins()`, `stride()` and
  `footprint_bytes()` are new.
- The arena is move-only.
- `detail::migrate_input_history()` now copies the carried spectra from one
  arena into the other instead of swapping vectors. It still allocates
  nothing, so it remains safe on the audio thread. Its cost is bounded by the
  carried ring length times `fft_size / 2 + 1` bins.

## Effect on output

None: `PartitionedConvolver` output is bit-identical. The MAC and the inverse
read the same bins in the same order.

## Effect on speed

A single convolver with a long IR runs faster, and several convolvers side by
side run much faster: the spectra now cover half the memory they did. In a few
cells where one convolver's spectra already fit in the L2 cache, it is a few
percent slower. See the pull request for the full table.

## Migration steps

Only code that builds a `ConvolverIrState` by hand, or reaches into its
spectra with vector operations, needs changes.

- Replace `ir_spectra.resize(n)` followed by per-partition `assign(...)` with
  `ir_spectra.assign(n, fft_size / 2 + 1)`. Then write each partition through
  `ir_spectra[p]`, for example `std::copy_n(src, ir_spectra.bins(),
  ir_spectra[p].begin())`.
- Do the same for `history->spectra`. A freshly assigned arena is zeroed.
- Replace per-slot `swap()`, `resize()` or `push_back()` with
  `assign(count, bins)` sized up front.
- Replace a loop that zero-fills every spectrum with `fill_zero()`.
- A spectrum holds `fft_size / 2 + 1` bins. Code that wrote the upper half of
  a full-width transform must keep only DC..Nyquist.

The super-convolver example's sliced IR builder
(`examples/super-convolver/sliced_ir_rebuild.hpp`) shows the pattern. It takes
both arenas in a channel's first tick and keeps each partition's FFT as its own
bounded slice.
