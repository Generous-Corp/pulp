#pragma once

#include <cstdint>
#include <string>

#include <pulp/audio/buffer.hpp>

namespace pulp::gpu_audio {

/// What the transport does for a block when the GPU result is not ready in
/// time (the worker fell behind, the device was lost, or the node is priming).
enum class MissPolicy {
    /// Output silence for the missed block (+ telemetry). The safe default: a
    /// dropout is obvious, bounded, and does not corrupt the timeline.
    Silence,

    /// Output the dry input for the missed block.
    ///
    /// **Timing-discontinuous — do not use in new nodes.** The transport's
    /// output is delayed by `latency_blocks * block_size`, so at output time `t`
    /// the correct sample is the wet result for `t - latency`. This policy
    /// substitutes the dry sample for `t` instead, which jumps the stream
    /// FORWARD by the whole latency for one block and then jumps back. That is
    /// audible as a glitch and, worse, it is a timeline break rather than an
    /// honest dropout.
    ///
    /// A correct dry substitute would be the dry input *delayed by the same
    /// latency*. Nothing here holds that history, so there is no correct dry
    /// substitute to give. Prefer `Silence`, or `CpuFallback` with a
    /// continuously-primed, latency-aligned fallback (see `prime_fallback`).
    PassthroughDry,

    /// Run the node's CPU fallback for the missed block. Requires
    /// `supports_cpu_fallback` AND a fallback that is kept continuously fed and
    /// latency-aligned via `prime_fallback()` — otherwise the substitute is a
    /// stale block with a torn history.
    CpuFallback,
};

/// Declared, fixed scheduling properties of a GPU audio node. Set at prepare()
/// time and stable for the lifetime of a prepared node — the transport reports
/// `latency_blocks * block_size` to the host as plugin delay compensation.
struct GpuAudioNodeDescriptor {
    std::string name;
    uint32_t input_channels = 0;
    uint32_t output_channels = 0;
    uint32_t block_size = 0;
    uint32_t sample_rate = 0;
    /// Fixed latency in host blocks. The transport delays output by this many
    /// blocks so the audio thread never waits on the GPU.
    uint32_t latency_blocks = 1;

    // Both default to FAILING CLOSED. A node that says nothing about its miss
    // behavior gets a bounded, obvious dropout — not a timeline break, and not a
    // CPU fallback it never implemented. Passing audio through on a miss has to
    // be a deliberate, stated choice, because a node author who never thought
    // about misses is exactly the author who has not made the fallback correct.
    MissPolicy miss_policy = MissPolicy::Silence;
    bool supports_cpu_fallback = false;
};

/// A unit of audio work the transport schedules onto a non-real-time worker.
///
/// Lifecycle:
/// - descriptor() / prepare() / release(): host thread, audio stopped.
/// - process_block(): the NON-real-time worker thread. May touch the GPU,
///   allocate scratch prepared in prepare(), and block on readback. It must
///   NOT be called from the audio callback.
/// - process_cpu_fallback(): real-time-safe; used by the transport for the
///   CpuFallback miss policy. Required if descriptor().supports_cpu_fallback.
class GpuAudioNode {
public:
    virtual ~GpuAudioNode() = default;

    virtual GpuAudioNodeDescriptor descriptor() const = 0;

    /// Non-RT. Allocate/compile/upload static data here.
    virtual bool prepare() = 0;

    /// Non-RT worker context. Process exactly one block (n == block_size).
    virtual void process_block(const audio::BufferView<const float>& input,
                               audio::BufferView<float>& output,
                               uint32_t n) = 0;

    /// RT-safe. Called by the transport on EVERY block (not just misses) for a
    /// CpuFallback node, BEFORE the output-ring read, so the node's CPU fallback
    /// can be kept continuously fed and its convolution history is always current
    /// the moment a miss makes it take over. A node whose fallback needs no live
    /// priming (the default) does nothing. MUST NOT allocate, lock, or block.
    virtual void prime_fallback(const audio::BufferView<const float>& /*input*/,
                                uint32_t /*n*/) noexcept {}

    /// RT-safe fallback. MUST NOT allocate, lock, or block — it runs on the
    /// audio thread for the CpuFallback miss policy (hence noexcept). Default
    /// copies input to output (dry) so a node without a real fallback still
    /// degrades safely. When paired with prime_fallback(), this emits the
    /// latency-aligned block the continuously-fed fallback has already computed.
    virtual void process_cpu_fallback(const audio::BufferView<const float>& input,
                                      audio::BufferView<float>& output,
                                      uint32_t n) noexcept {
        const uint32_t ch = output.num_channels() < input.num_channels()
                                ? output.num_channels()
                                : input.num_channels();
        for (uint32_t c = 0; c < ch; ++c) {
            const float* src = input.channel_ptr(c);
            float* dst = output.channel_ptr(c);
            for (uint32_t i = 0; i < n; ++i) dst[i] = src[i];
        }
    }
};

} // namespace pulp::gpu_audio
