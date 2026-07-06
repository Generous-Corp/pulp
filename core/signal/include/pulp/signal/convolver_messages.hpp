#pragma once

#include <pulp/runtime/spsc_queue.hpp>
#include <pulp/signal/fft.hpp>

#include <algorithm>
#include <atomic>
#include <complex>
#include <cstddef>
#include <memory>
#include <vector>

namespace pulp::signal {

/// Pre-computed, audio-thread-ready state for a single impulse response.
///
/// Built off the audio thread (FFTs of every partition, working buffers
/// sized to match), then atomically handed to a `PartitionedConvolver`
/// for zero-pop swap-in at the next block boundary.
///
/// Owned by `ConvolverIrSwapper` until claimed by the audio thread.
template <typename SampleType = float>
struct ConvolverIrStateT {
    int block_size = 0;
    int fft_size = 0;
    std::size_t num_partitions = 0;

    std::unique_ptr<FftT<SampleType>> fft;
    std::vector<std::vector<std::complex<SampleType>>> ir_spectra;
    std::vector<std::vector<std::complex<SampleType>>> input_spectra;
    std::vector<std::complex<SampleType>> input_buffer;
    std::vector<std::complex<SampleType>> accum;
};

using ConvolverIrState = ConvolverIrStateT<float>;
using ConvolverIrState64 = ConvolverIrStateT<double>;

namespace detail {

/// Build a `ConvolverIrState` from a raw IR. This is the expensive path
/// (allocations + N forward FFTs) and must run off the audio thread.
template <typename SampleType = float>
inline std::unique_ptr<ConvolverIrStateT<SampleType>>
build_convolver_ir_state(const SampleType* ir,
                         std::size_t ir_length,
                         std::size_t block_size) {
    if (ir == nullptr || ir_length == 0)
        return nullptr;

    // Round up to next power-of-two for radix-2 FFT, matching
    // PartitionedConvolver::load_ir.
    if (block_size == 0 || (block_size & (block_size - 1)) != 0) {
        std::size_t pot = 1;
        while (pot < block_size) pot <<= 1;
        block_size = pot;
    }

    auto state = std::make_unique<ConvolverIrStateT<SampleType>>();
    state->block_size = static_cast<int>(block_size);
    state->fft_size = state->block_size * 2;
    state->fft = std::make_unique<FftT<SampleType>>(state->fft_size);
    state->num_partitions = (ir_length + block_size - 1) / block_size;

    state->ir_spectra.resize(state->num_partitions);
    std::vector<std::complex<SampleType>> padded(
        state->fft_size, {SampleType{0.0f}, SampleType{0.0f}});

    for (std::size_t p = 0; p < state->num_partitions; ++p) {
        const std::size_t offset = p * block_size;
        const std::size_t count = std::min(block_size, ir_length - offset);

        std::fill(padded.begin(), padded.end(),
                  std::complex<SampleType>{SampleType{0.0f}, SampleType{0.0f}});
        for (std::size_t i = 0; i < count; ++i)
            padded[i] = {ir[offset + i], SampleType{0.0f}};

        state->ir_spectra[p].assign(padded.begin(), padded.end());
        state->fft->forward(state->ir_spectra[p].data());
    }

    state->input_buffer.assign(state->fft_size, {0.0f, 0.0f});
    state->input_spectra.assign(
        state->num_partitions,
        std::vector<std::complex<SampleType>>(
            state->fft_size, {SampleType{0.0f}, SampleType{0.0f}}));
    state->accum.assign(state->fft_size,
                        {SampleType{0.0f}, SampleType{0.0f}});

    return state;
}

/// Carry the input frequency-domain delay line (and the time-domain overlap
/// buffer) from a displaced IR state into a freshly-built one during a live swap.
///
/// The input FDL is a ring of the FFTs of recent input blocks — it depends only
/// on the audio, not the IR — so replacing the whole state at swap would zero it
/// and force the first blocks after the swap to convolve against silent history,
/// an audible dip / tail truncation. This moves the most recent min(prev, next)
/// partitions of history into `next` (age-aligned) and carries the overlap
/// buffer, so a swap is genuinely continuous.
///
/// `prev` is the displaced state, which the caller retires immediately after, so
/// this SWAPS the buffers out of it rather than copying: O(num_partitions) pointer
/// swaps, no per-sample copy and no allocation, keeping the audio-thread swap
/// cheap even for a long (many-partition) IR. `prev` is left holding `next`'s old
/// zero buffers — harmless, it is about to be freed off-thread. Requires matching
/// block/FFT sizes.
///
/// `old_write_pos` is the convolver's ring cursor at the swap — the slot the NEXT
/// input block would occupy — so the most-recent written block is at old-1.
/// Returns the new ring cursor to use after the swap (always 0: the carried
/// history is laid out so the next write lands at 0).
template <typename SampleType = float>
inline std::size_t carry_input_history(ConvolverIrStateT<SampleType>& prev,
                                       ConvolverIrStateT<SampleType>& next,
                                       std::size_t old_write_pos) {
    if (prev.fft_size != next.fft_size || prev.block_size != next.block_size)
        return 0;  // incompatible geometry — leave `next` fresh (still no crash)
    const std::size_t P = prev.num_partitions;
    const std::size_t Q = next.num_partitions;
    if (P == 0 || Q == 0) return 0;
    const std::size_t K = std::min(P, Q);
    // Age 0 = most recently written block. With the next write landing at cursor 0,
    // an age-a block belongs at ring position (Q - 1 - a); older-than-available
    // ages stay at the freshly-zeroed value. This keeps process()'s readback
    // `(cursor + Q - p) % Q` reading the same audio for every partition that exists.
    // Each `a` maps to a distinct (oldi, newi) in two distinct states, so swapping
    // never aliases a slot a later iteration reads.
    for (std::size_t a = 0; a < K; ++a) {
        const std::size_t oldi = (old_write_pos + P - 1 - a) % P;
        const std::size_t newi = (Q - 1 - a) % Q;
        next.input_spectra[newi].swap(prev.input_spectra[oldi]);
    }
    // The overlap buffer's lower half holds the previous block's samples (the
    // overlap the next block's FFT needs); it is IR-independent, so carry it too.
    next.input_buffer.swap(prev.input_buffer);
    return 0;
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

    ~ConvolverIrSwapperT() {
        // Reclaim anything still parked so we don't leak memory at
        // teardown. Safe because both producer and consumer threads
        // must have stopped by destruction.
        delete pending_.exchange(nullptr, std::memory_order_acquire);
        while (auto* p = pop_retired_raw()) delete p;
    }

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
        if (!next)
            return false;
        // Release ordering so the state's contents (FFTs, buffers)
        // happen-before any acquire-load on the audio thread.
        auto* raw = next.release();
        auto* prev = pending_.exchange(raw, std::memory_order_acq_rel);
        // If the audio thread hasn't consumed the previous staging,
        // reclaim it here on the (non-RT) staging thread.
        delete prev;
        return true;
    }

    /// Audio-thread-callable: atomically claim the most recently staged
    /// IR, if any. Returns nullptr if nothing is pending. NEVER blocks,
    /// NEVER allocates, NEVER frees.
    std::unique_ptr<ConvolverIrStateT<SampleType>> try_consume() {
        auto* raw = pending_.exchange(nullptr, std::memory_order_acquire);
        return std::unique_ptr<ConvolverIrStateT<SampleType>>(raw);
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
        if (!displaced) return true; // nothing to park
        auto* raw = displaced.get();
        if (!retired_queue_.try_push(raw)) return false;
        (void)displaced.release(); // ownership transferred to queue
        return true;
    }

    /// True if there's room in the retire ring for one more displaced
    /// IR. Audio thread checks this before consuming pending so a
    /// swap never strands a displaced IR without an off-thread free
    /// path.
    bool has_retire_capacity() const {
        return retired_queue_.size_approx() < kRetireRingCapacity;
    }

    /// Room to park at least @p n IRs — for a caller that must retire more than
    /// one in a single audio-thread step (e.g. a completed crossfade fade-out
    /// PLUS the just-displaced IR). Reserve before committing so no retire ever
    /// fails inline and strands an IR on the audio thread.
    bool has_retire_capacity(std::size_t n) const {
        return retired_queue_.size_approx() + n <= kRetireRingCapacity;
    }

    /// UI / worker thread: reclaim ALL retired IRs the audio thread
    /// has parked. Returns the count freed; the actual deallocation
    /// runs through `unique_ptr` destructors in this scope (off-RT).
    /// Test callers can use the single-pop overload below if they
    /// want to inspect each freed state.
    std::size_t drain_old() {
        std::size_t freed = 0;
        while (auto* raw = pop_retired_raw()) {
            std::unique_ptr<ConvolverIrStateT<SampleType>> state(raw);
            ++freed;
        }
        return freed;
    }

    /// UI / worker thread: pop ONE retired IR for test inspection.
    /// Returns nullptr if none queued.
    std::unique_ptr<ConvolverIrStateT<SampleType>> drain_old_one() {
        auto* raw = pop_retired_raw();
        return std::unique_ptr<ConvolverIrStateT<SampleType>>(raw);
    }

    /// True if a freshly-staged IR is awaiting consumption.
    bool has_pending() const {
        return pending_.load(std::memory_order_acquire) != nullptr;
    }

    /// True if any retired IR is awaiting drain.
    bool has_retired() const {
        return retired_queue_.size_approx() > 0;
    }

    /// Compile-time capacity of the retired-IR ring. Sized so a few
    /// back-to-back swaps survive a single missed drain tick — past
    /// this point, swaps refuse until a drain runs.
    static constexpr std::size_t retire_capacity() {
        return kRetireRingCapacity;
    }

private:
    static constexpr std::size_t kRetireRingCapacity = 8;

    // Producer → consumer: latest IR awaiting pickup.
    std::atomic<ConvolverIrStateT<SampleType>*> pending_{nullptr};
    // Consumer → producer: ring of displaced IRs awaiting off-thread
    // deletion. Replaces the single-slot atomic the original impl used
    // so rapid IR automation can queue multiple displaced states
    // without forcing audio-thread frees when one drain tick is missed.
    pulp::runtime::SpscQueue<ConvolverIrStateT<SampleType>*,
                             kRetireRingCapacity> retired_queue_;

    ConvolverIrStateT<SampleType>* pop_retired_raw() {
        auto popped = retired_queue_.try_pop();
        return popped ? *popped : nullptr;
    }
};

using ConvolverIrSwapper = ConvolverIrSwapperT<float>;
using ConvolverIrSwapper64 = ConvolverIrSwapperT<double>;

} // namespace pulp::signal
