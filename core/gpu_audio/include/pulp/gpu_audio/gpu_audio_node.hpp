#pragma once

#include <cstdint>
#include <string>

#include <pulp/audio/buffer.hpp>

namespace pulp::gpu_audio {

/// What the transport does for a block when the GPU result is not ready in
/// time (the worker fell behind, the device was lost, or the node is priming).
enum class MissPolicy {
    Silence,        ///< Output silence for the missed block (+ telemetry).
    PassthroughDry, ///< Output the dry input for the missed block.
    CpuFallback,    ///< Run the node's CPU fallback for the missed block.
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
    MissPolicy miss_policy = MissPolicy::PassthroughDry;
    bool supports_cpu_fallback = true;
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
