#pragma once

#include <pulp/runtime/slot.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <atomic>
#include <complex>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <span>
#include <vector>

namespace pulp::signal {

/// A fixed number of equal-length spectra in one contiguous, 64-byte-aligned
/// block, each starting on a 64-byte boundary.
///
/// The partitioned convolver keeps its IR partition spectra and its input
/// history in these. One block per set keeps the per-block multiply-accumulate
/// walking a compact address range: separately allocated spectra, or spectra
/// padded to the full FFT width, spread the same data over twice the memory,
/// which costs most when several convolvers run side by side.
///
/// `operator[]` returns a span of `bins()` values. Allocation happens only in
/// the sizing constructor and `assign()`; everything else is allocation-free.
template <typename SampleType = float> class ConvolverSpectrumArenaT {
  public:
    using value_type = std::complex<SampleType>;

    /// Distance between consecutive spectra in `value_type`s: `bins` rounded
    /// up to a whole number of 64-byte lines.
    static constexpr std::size_t stride_for(std::size_t bins) noexcept {
        constexpr std::size_t per_line = 64 / sizeof(value_type);
        return (bins + per_line - 1) / per_line * per_line;
    }

    ConvolverSpectrumArenaT() = default;
    ConvolverSpectrumArenaT(std::size_t count, std::size_t bins) {
        assign(count, bins);
    }

    // The alignment offset is relative to the storage address, which a copy
    // would change; a move keeps the buffer, so moving is safe.
    ConvolverSpectrumArenaT(const ConvolverSpectrumArenaT&) = delete;
    ConvolverSpectrumArenaT& operator=(const ConvolverSpectrumArenaT&) = delete;
    ConvolverSpectrumArenaT(ConvolverSpectrumArenaT&&) noexcept = default;
    ConvolverSpectrumArenaT& operator=(ConvolverSpectrumArenaT&&) noexcept = default;

    /// Allocate `count` zeroed spectra of `bins` values each.
    void assign(std::size_t count, std::size_t bins) {
        constexpr std::size_t per_line = 64 / sizeof(value_type);
        count_ = count;
        bins_ = bins;
        stride_ = stride_for(bins);
        storage_.assign(count_ * stride_ + per_line, value_type{});
        const auto address = reinterpret_cast<std::uintptr_t>(storage_.data());
        offset_ = ((64u - address % 64u) % 64u) / sizeof(value_type);
    }

    std::size_t size() const noexcept {
        return count_;
    }
    bool empty() const noexcept {
        return count_ == 0;
    }
    std::size_t bins() const noexcept {
        return bins_;
    }
    std::size_t stride() const noexcept {
        return stride_;
    }
    /// Bytes this arena holds, including its alignment slack.
    std::size_t footprint_bytes() const noexcept {
        return storage_.size() * sizeof(value_type);
    }

    std::span<value_type> operator[](std::size_t i) noexcept {
        return {storage_.data() + offset_ + i * stride_, bins_};
    }
    std::span<const value_type> operator[](std::size_t i) const noexcept {
        return {storage_.data() + offset_ + i * stride_, bins_};
    }

    /// Zero every spectrum. Allocation-free.
    void fill_zero() noexcept {
        std::fill(storage_.begin(), storage_.end(), value_type{});
    }

  private:
    std::vector<value_type> storage_;
    std::size_t offset_ = 0, count_ = 0, bins_ = 0, stride_ = 0;
};

/// The convolver's input-side overlap-save history.
///
/// This belongs to the INPUT STREAM, not to any impulse response: it is the ring
/// of forward FFTs of recent input blocks plus the time-domain overlap buffer,
/// and what is in it depends only on the audio that has already been fed in.
/// Its identity therefore does not change when the IR changes.
///
/// `PartitionedConvolver` owns exactly one of these and renders EVERY live IR
/// against it — including both sides of a crossfade — so an IR swap never
/// restarts the history and the incoming IR never has to convolve against
/// silence. It is also why a crossfade costs one forward FFT per block rather
/// than two.
///
/// Ring capacity (`spectra.size()`) must be at least the partition count of
/// every IR rendering against it, so that the ages an IR reads back map to
/// distinct slots. A `ConvolverIrStateT` carries a pre-allocated history sized
/// for its own partition count as a SPARE; the convolver adopts that spare only
/// when it needs a longer ring than it already has, which is what keeps ring
/// growth allocation-free on the audio thread.
template <typename SampleType = float>
struct ConvolverInputHistoryT {
    int block_size = 0;
    int fft_size = 0;

    /// Ring of forward FFTs of recent input blocks. Each slot holds the
    /// `fft_size / 2 + 1` bins DC..Nyquist, the only ones written and read:
    /// the input is real, so the upper half of its spectrum is redundant.
    ///
    /// `write_pos` is advanced once per processed block, after every IR has
    /// rendered, so it means two different things depending on when you look —
    /// get this wrong and the delay line is silently off by one:
    ///   - DURING a block, it is the slot the block being processed occupies, so
    ///     the block of age `a` sits at `(write_pos + capacity - a) % capacity`
    ///     and partition `p` of an IR reads age `p`;
    ///   - BETWEEN blocks (where a swap happens), it is the slot the NEXT block
    ///     will occupy, so the most recently written block is at `write_pos - 1`.
    ConvolverSpectrumArenaT<SampleType> spectra;
    /// Overlap-save window: `fft_size` complex slots whose leading `fft_size`
    /// scalars hold the real window, previous block first, then the current
    /// one. Zero-filled is a valid cold start.
    std::vector<std::complex<SampleType>> overlap;
    std::size_t write_pos = 0;
};

using ConvolverInputHistory = ConvolverInputHistoryT<float>;
using ConvolverInputHistory64 = ConvolverInputHistoryT<double>;

/// Pre-computed, audio-thread-ready state for a single impulse response.
///
/// Built off the audio thread (FFTs of every partition, working buffers
/// sized to match), then atomically handed to a `PartitionedConvolver`
/// for zero-pop swap-in at the next block boundary.
///
/// Holds only what is genuinely per-IR: the partition spectra, the transform,
/// and the per-render accumulator. The input delay line is NOT here — it lives
/// in `ConvolverInputHistoryT`, owned by the convolver, because it outlives any
/// individual IR (see that type). `history` is a pre-allocated spare sized for
/// this IR's partition count, which the convolver adopts only if its current
/// ring is too short or has different geometry; otherwise the spare is handed
/// back out to be freed off the audio thread.
///
/// A state built by hand rather than by `build_convolver_ir_state` must still
/// populate `history` (with `spectra.size() >= num_partitions` and `overlap`
/// sized to `fft_size`), or the convolver cannot guarantee a ring long enough
/// for it and falls back to passing audio through unconvolved. Both
/// `ir_spectra` and the history's `spectra` hold `fft_size / 2 + 1` bins per
/// spectrum.
///
/// Owned by `ConvolverIrSwapper` until claimed by the audio thread.
template <typename SampleType = float>
struct ConvolverIrStateT {
    int block_size = 0;
    int fft_size = 0;
    std::size_t num_partitions = 0;

    std::unique_ptr<FftT<SampleType>> fft;
    /// One spectrum per partition, `fft_size / 2 + 1` bins (DC..Nyquist).
    /// Storing only these keeps the MAC's working set to the bins it reads.
    ConvolverSpectrumArenaT<SampleType> ir_spectra;
    std::vector<std::complex<SampleType>> accum;
    std::unique_ptr<ConvolverInputHistoryT<SampleType>> history;
};

using ConvolverIrState = ConvolverIrStateT<float>;
using ConvolverIrState64 = ConvolverIrStateT<double>;

namespace detail {

/// The block size a convolver prepared for `block_size` accepts: the request
/// rounded up to the next power of two (the radix-2 FFT needs it), with 0
/// treated as 1. Every fixed-block convolver uses this one rule so their
/// accepted block sizes cannot drift apart.
constexpr std::size_t convolver_block_size(std::size_t block_size) noexcept {
    std::size_t pot = 1;
    while (pot < block_size && pot <= (static_cast<std::size_t>(-1) >> 1))
        pot <<= 1;
    return pot;
}

/// Build a `ConvolverIrState` from a raw IR. This is the expensive path
/// (allocations + N forward FFTs) and must run off the audio thread.
template <typename SampleType = float>
inline std::unique_ptr<ConvolverIrStateT<SampleType>>
build_convolver_ir_state(const SampleType* ir,
                         std::size_t ir_length,
                         std::size_t block_size) {
    if (ir == nullptr || ir_length == 0)
        return nullptr;

    block_size = convolver_block_size(block_size);

    auto state = std::make_unique<ConvolverIrStateT<SampleType>>();
    state->block_size = static_cast<int>(block_size);
    state->fft_size = state->block_size * 2;
    state->fft = std::make_unique<FftT<SampleType>>(state->fft_size);
    state->num_partitions = (ir_length + block_size - 1) / block_size;

    const auto bins = static_cast<std::size_t>(state->fft_size / 2 + 1);
    state->ir_spectra.assign(state->num_partitions, bins);
    std::vector<std::complex<SampleType>> padded(
        state->fft_size, {SampleType{0.0f}, SampleType{0.0f}});

    for (std::size_t p = 0; p < state->num_partitions; ++p) {
        const std::size_t offset = p * block_size;
        const std::size_t count = std::min(block_size, ir_length - offset);

        std::fill(padded.begin(), padded.end(),
                  std::complex<SampleType>{SampleType{0.0f}, SampleType{0.0f}});
        for (std::size_t i = 0; i < count; ++i)
            padded[i] = {ir[offset + i], SampleType{0.0f}};

        state->fft->forward(padded.data());
        std::copy_n(padded.begin(), bins, state->ir_spectra[p].begin());
    }

    state->accum.assign(state->fft_size,
                        {SampleType{0.0f}, SampleType{0.0f}});
    state->history = std::make_unique<ConvolverInputHistoryT<SampleType>>();
    state->history->block_size = state->block_size;
    state->history->fft_size = state->fft_size;
    state->history->overlap.assign(state->fft_size, {0.0f, 0.0f});
    state->history->spectra.assign(state->num_partitions, bins);

    return state;
}

/// Migrate the recorded input history from `from` into `to`, age-aligned.
///
/// Used when the convolver must move to a longer (or differently-shaped) ring
/// than the one it is holding: the incoming ring is a freshly-built spare, so
/// everything the old ring recorded has to be carried across or the input
/// stream restarts from silence.
///
/// The carried spectra are copied from one arena into the other: no allocation,
/// and work bounded by the carried ring length times `fft_size / 2 + 1` bins
/// (about 0.8 MB for a 96000-tap IR at 64-sample blocks), so this is safe on the
/// audio thread. `from` keeps its own copy, which is harmless: the caller hands
/// `from` off to be freed off the audio thread.
///
/// `from.write_pos` is the slot the next block would be written to, so the
/// most-recent (age 0) block is at `from.write_pos - 1`. History is laid out in
/// `to` so that the next write lands at slot 0, and `to.write_pos` is set to 0
/// accordingly. Ages older than the shorter of the two rings can hold are
/// dropped; they are beyond the reach of any IR that ring can serve.
///
/// A geometry mismatch (different block or FFT size) leaves `to` cold, which is
/// the only correct answer: spectra taken at one FFT size cannot be reinterpreted
/// at another.
template <typename SampleType = float>
inline void migrate_input_history(ConvolverInputHistoryT<SampleType>& from,
                                  ConvolverInputHistoryT<SampleType>& to) {
    if (from.fft_size != to.fft_size || from.block_size != to.block_size)
        return;
    const std::size_t old_capacity = from.spectra.size();
    const std::size_t new_capacity = to.spectra.size();
    if (old_capacity == 0 || new_capacity == 0) return;
    const std::size_t carried = std::min(old_capacity, new_capacity);
    // Both rings share the FFT size, so their spectra have the same length.
    // Each `a` maps to a distinct slot in each of two distinct rings, so a copy
    // never overwrites a slot a later iteration reads.
    for (std::size_t a = 0; a < carried; ++a) {
        const std::size_t oldi = (from.write_pos + old_capacity - 1 - a) % old_capacity;
        const std::size_t newi = (new_capacity - 1 - a) % new_capacity;
        const auto source = from.spectra[oldi];
        std::copy(source.begin(), source.end(), to.spectra[newi].begin());
    }
    // The overlap buffer's lower half holds the previous block's samples (the
    // overlap the next block's FFT needs); it is IR-independent, so carry it too.
    to.overlap.swap(from.overlap);
    to.write_pos = 0;
}

} // namespace detail

/// Lock-free background-IR shuttle for `PartitionedConvolver`.
///
/// Workflow (single producer, single audio-thread consumer):
///
///   ConvolverIrSwapper swapper;          // background / UI thread
///   PartitionedConvolver conv;           // audio thread
///   conv.load_ir(initial_ir, len, 256);  // first IR loaded inline
///
///   // ── UI / worker thread ──
///   swapper.stage_ir(new_ir, new_len, 256);  // allocates, FFTs
///
///   // ── audio thread (block boundary) ──
///   conv.try_swap_ir(swapper);  // atomically picks up new IR
///
///   // ── UI / worker thread ──
///   swapper.drain_old();  // reclaim memory from the displaced IR
///
/// Guarantees:
///   - Audio thread does ZERO allocation, ZERO FFT precompute work,
///     and ZERO blocking on a mutex; just two atomic pointer ops.
///   - Old IR ownership is transferred back to the swapper via a
///     reverse atomic slot; the UI/worker thread reclaims it with
///     `drain_old()`. Audio thread never frees memory.
template <typename SampleType = float>
class ConvolverIrSwapperT {
public:
    ConvolverIrSwapperT() = default;

    ConvolverIrSwapperT(const ConvolverIrSwapperT&) = delete;
    ConvolverIrSwapperT& operator=(const ConvolverIrSwapperT&) = delete;

    /// Build a fresh IR state off the audio thread and publish it for
    /// the audio thread to pick up. Returns `true` on success.
    ///
    /// If a previously-staged IR has not yet been consumed by the
    /// audio thread, this call replaces it and the old pre-built
    /// state is freed inline (still on the worker thread — never the
    /// audio thread).
    bool stage_ir(const SampleType* ir,
                  std::size_t ir_length,
                  std::size_t block_size) {
        auto next = detail::build_convolver_ir_state(ir, ir_length, block_size);
        if (!next)
            return false;
        return stage_ir(std::move(next));
    }

    /// Publish a pre-built state. Same semantics as the raw overload.
    bool stage_ir(std::unique_ptr<ConvolverIrStateT<SampleType>> next) {
        return handoff_.publish(std::move(next));
    }

    /// Audio-thread-callable: atomically claim the most recently staged
    /// IR, if any. Returns nullptr if nothing is pending. NEVER blocks,
    /// NEVER allocates, NEVER frees.
    std::unique_ptr<ConvolverIrStateT<SampleType>> try_consume() {
        return handoff_.try_consume();
    }

    /// Audio-thread-callable: park the IR that the audio thread is
    /// displacing so a non-RT thread can free it. Returns `true` if
    /// the IR was queued for drain; `false` if the retire ring is
    /// already full. On `false` the displaced state is returned to
    /// the caller via the moved-in `unique_ptr` being LEFT INTACT
    /// (caller still owns it). The audio-thread caller (see
    /// `PartitionedConvolver::try_swap_ir`) should gate the swap on
    /// `has_retire_capacity()` first so this path is never hit in
    /// well-paced operation.
    ///
    /// RT contract: never allocates, never blocks, never frees.
    [[nodiscard]] bool retire(std::unique_ptr<ConvolverIrStateT<SampleType>>& displaced) {
        return handoff_.retire(displaced);
    }

    /// True if there's room in the retire ring for one more displaced
    /// IR. Audio thread checks this before consuming pending so a
    /// swap never strands a displaced IR without an off-thread free
    /// path.
    bool has_retire_capacity() const { return handoff_.has_retire_capacity(); }

    /// Room to park at least @p n IRs — for a caller that must retire more than
    /// one in a single audio-thread step (e.g. a completed crossfade fade-out
    /// PLUS the just-displaced IR). Reserve before committing so no retire ever
    /// fails inline and strands an IR on the audio thread.
    bool has_retire_capacity(std::size_t n) const { return handoff_.has_retire_capacity(n); }

    /// UI / worker thread: reclaim ALL retired IRs the audio thread
    /// has parked. Returns the count freed; the actual deallocation
    /// runs off the audio thread.
    std::size_t drain_old() { return handoff_.drain_retired(); }

    /// UI / worker thread: pop ONE retired IR for test inspection.
    /// Returns nullptr if none queued.
    std::unique_ptr<ConvolverIrStateT<SampleType>> drain_old_one() {
        return handoff_.drain_retired_one();
    }

    /// True if a freshly-staged IR is awaiting consumption.
    bool has_pending() const { return handoff_.has_pending(); }

    /// True if any retired IR is awaiting drain.
    bool has_retired() const { return handoff_.has_retired(); }

    /// Compile-time capacity of the retired-IR ring. Sized so a few
    /// back-to-back swaps survive a single missed drain tick — past
    /// this point, swaps refuse until a drain runs.
    static constexpr std::size_t retire_capacity() { return kRetireRingCapacity; }

private:
    static constexpr std::size_t kRetireRingCapacity = 8;

    // The publish/consume/retire machinery is `runtime::Handoff` — the shared
    // real-time ownership-transfer primitive. It owns the pending slot, the
    // retire ring, and teardown reclamation, so this class only adds the
    // IR-specific `stage_ir` build step and the `drain_old` naming its callers
    // already use.
    pulp::runtime::Handoff<ConvolverIrStateT<SampleType>, kRetireRingCapacity> handoff_;
};

using ConvolverIrSwapper = ConvolverIrSwapperT<float>;
using ConvolverIrSwapper64 = ConvolverIrSwapperT<double>;

} // namespace pulp::signal
