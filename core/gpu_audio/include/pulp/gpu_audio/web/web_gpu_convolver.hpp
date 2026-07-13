#pragma once

#include <chrono>
#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include <pulp/render/gpu_compute.hpp>

namespace pulp::gpu_audio {

/// Submit/collect FFT convolver for a single-threaded event loop.
///
/// Same math as GpuConvolver (zero-padded complex block → fused GPU
/// forward-FFT / complex-multiply by the resident IR spectrum / inverse-FFT →
/// overlap-add carry), with two structural differences that a browser forces:
///
///  1. NO GPU READBACK IN prepare(). GpuConvolver builds the IR spectrum with
///     the BLOCKING GpuCompute::fft_forward(). That call pumps the device event
///     queue until the map resolves — on a browser's event loop the pump IS the
///     loop, so it can never resolve its own map and would simply burn the
///     deadline. The IR transform is a one-time offline cost, so it is done on
///     the CPU (signal::FftT) and handed to prepare_convolution_batch(), which
///     is upload-only. One readback deleted from the critical path.
///
///  2. NO BLOCKING PER BLOCK. submit() issues convolve_batch_async() and
///     returns; collect() pumps GpuCompute::poll_readbacks() exactly ONCE (never
///     spins) and drains whatever landed. Up to `depth` blocks may be in flight,
///     each with its own scratch slot.
///
/// THE OVERLAP-ADD CARRY IS SEQUENTIAL STATE and is therefore applied in
/// collect(), in completion (== submission) order — never at submit time, where
/// blocks may be issued out of the order their results land in. A block that
/// Expires, Fails, or reads back non-finite does NOT land: it is reported to the
/// sink with ok == false and the carry is NOT mutated (mirroring
/// GpuConvolver::process_block, which on a failed readback emits silence and
/// deliberately leaves the carry alone — a stale result added to the carry would
/// poison every later block of that channel).
///
/// This class carries NO CPU fallback. In the browser the safety net lives in
/// the plugin, on the audio side of the ring, because the GPU work runs in a
/// DedicatedWorker with its own wasm heap — a fallback here would be in the
/// wrong linear memory and, worse, on the wrong thread.
///
/// Not real-time-safe: submit/collect from the GPU worker, never from the audio
/// callback.
class WebGpuConvolver {
public:
    /// Blocks that may be in flight at once. 3 keeps the GPU fed across one
    /// round-trip of map latency without letting a stalled tab queue unbounded
    /// work — every in-flight block is one more block of scratch, and one more
    /// block that must expire before its deadline is honoured.
    static constexpr uint32_t kDefaultDepth = 3;

    /// Called once per finished block, in submission order. `out_planar` holds
    /// `channels * block` floats (channel-major); it is silence when ok is
    /// false. The pointer is owned by the convolver and valid only for the
    /// duration of the call.
    using Sink = std::function<void(uint32_t seq, const float* out_planar, bool ok)>;

    explicit WebGpuConvolver(uint32_t depth = kDefaultDepth);
    ~WebGpuConvolver();

    WebGpuConvolver(const WebGpuConvolver&) = delete;
    WebGpuConvolver& operator=(const WebGpuConvolver&) = delete;

    /// Allocate scratch, transform the IR on the CPU, and upload the resident IR
    /// spectrum. `gpu` must already be initialized and must outlive this object.
    /// No GPU readback happens here (see the class comment).
    bool prepare(render::GpuCompute* gpu, int sample_rate, int block, int channels,
                 const float* ir, int ir_len);

    /// Issue one block. `in_planar` holds `channels * block` floats
    /// (channel-major) and is consumed (copied into this block's slot) before
    /// returning. Returns false — allocating nothing — when the pipeline is at
    /// its depth cap, the device is gone, `frames` is not the prepared block
    /// size, or the GPU rejects the dispatch.
    bool submit(uint32_t seq, const float* in_planar, int frames,
                std::chrono::microseconds deadline);

    /// Harvest. Pumps poll_readbacks() ONCE and delivers every block that has
    /// resolved, in submission order, applying the overlap-add carry to the ones
    /// that landed. Returns the number of blocks handed to the sink.
    uint32_t collect(const Sink& sink);

    bool prepared() const { return prepared_; }
    /// Device loss is normal browser behaviour, not an init failure: the owner is
    /// expected to re-acquire a device and prepare() again.
    bool device_lost() const { return gpu_ != nullptr && gpu_->device_lost(); }
    uint32_t fft_size() const { return fft_size_; }
    uint32_t depth() const { return depth_; }
    uint32_t in_flight() const;

    // Per-pipeline counters. GpuCompute::async_stats() counts the same events
    // device-wide; these are this convolver's view of them, plus the one status
    // the device cannot see — a readback that landed non-finite (counted failed).
    uint64_t submits() const;
    uint64_t resolves() const;   ///< maps that landed with a usable payload
    uint64_t expired() const;    ///< deadline passed before the map resolved
    uint64_t failed() const;     ///< map error, device loss, or non-finite payload

private:
    enum class SlotState : uint8_t { Free, InFlight, Ready, Bad };

    struct Slot {
        SlotState state = SlotState::Free;
        uint32_t seq = 0;
        std::vector<float> in_pad;  ///< 2 * fft_size * channels, interleaved complex
        std::vector<float> time;    ///< 2 * fft_size * channels, inverse result
    };

    /// Slots + counters live behind a shared_ptr because an in-flight readback's
    /// `dest` points INTO a slot and GpuCompute writes it before firing the
    /// completion. The completion holds a strong reference, so the storage
    /// outlives this object whenever a request is still in flight — otherwise a
    /// convolver destroyed with work outstanding would be written through.
    struct State {
        std::vector<Slot> slots;
        std::vector<uint32_t> pending;  ///< ring of slot indices, submission order
        uint32_t head = 0;
        uint32_t count = 0;

        uint64_t submits = 0;
        uint64_t resolves = 0;
        uint64_t expired = 0;
        uint64_t failed = 0;
    };

    std::shared_ptr<State> state_;
    render::GpuCompute* gpu_ = nullptr;

    uint32_t depth_ = kDefaultDepth;
    uint32_t channels_ = 0;
    uint32_t block_ = 0;
    uint32_t sample_rate_ = 0;
    uint32_t fft_size_ = 0;
    bool prepared_ = false;

    std::vector<float> ir_spec_;             ///< 2 * fft_size interleaved complex
    std::vector<std::vector<float>> carry_;  ///< per-channel OLA accumulator
    std::vector<float> out_;                 ///< channels * block, handed to the sink
};

}  // namespace pulp::gpu_audio
