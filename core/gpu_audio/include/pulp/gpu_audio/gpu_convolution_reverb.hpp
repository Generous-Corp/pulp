#pragma once

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

#include <pulp/audio/buffer.hpp>
#include <pulp/gpu_audio/gpu_audio_capability.hpp>
#include <pulp/gpu_audio/gpu_audio_transport.hpp>
#include <pulp/gpu_audio/gpu_convolver.hpp>
#include <pulp/signal/zero_latency_convolver.hpp>

namespace pulp::gpu_audio {

/// Configuration for the opt-in GPU realization of Forge's stereo convolution
/// reverb.  The CPU catalog realization remains the default; callers must set
/// `gpu_enabled` explicitly before prepare().
struct GpuConvolutionReverbConfig {
    std::uint32_t block_size = 0;
    std::uint32_t sample_rate = 0;
    std::vector<std::vector<float>> impulse_response;
    signal::IrNormalizeMode normalize = signal::IrNormalizeMode::energy;
    double tail_trim_db = signal::ZeroLatencyConvolver::kTailTrimDbDefault;
    double tail_fade_ms = signal::ZeroLatencyConvolver::kTailFadeMsDefault;
    int resample_taps_per_phase = signal::ZeroLatencyConvolver::kResampTapsPerPhaseDefault;
    std::uint32_t ring_blocks = 8;
    bool gpu_enabled = false;
};

/// Per-lane evidence.  A dual-mono IR is represented by two concrete
/// GpuConvolver/GpuAudioTransport pairs; the wrapper never pretends to be a
/// concrete authenticated GPU node.
struct GpuConvolutionReverbLaneReport {
    GpuAudioCapabilityReport capability;
    GpuAudioTransport::Stats stats;
    GpuAudioTransport::DeliverySnapshot delivery;
};

struct GpuConvolutionReverbReport {
    bool prepared = false;
    bool gpu_enabled = false;
    bool authenticated_shared_provider = false;
    std::uint32_t block_size = 0;
    std::uint32_t latency_samples = 0;
    std::array<GpuConvolutionReverbLaneReport, 2> lanes{};
};

/// Optional, control-parity GPU route for the two-channel Forge convolution
/// reverb.  It uses two separately authenticated mono GpuConvolver instances,
/// one per output lane, so a two-channel IR remains dual-mono and distinct L/R
/// stimuli cannot accidentally collapse to one resident mono spectrum.
///
/// The wrapper owns only host-side control, filtering, predelay, dry alignment,
/// and lane composition.  GPU work and the continuously primed CPU fallbacks
/// remain inside the existing GpuAudioTransport instances.  Its fixed latency
/// is exactly two host blocks and is reported through latency_samples().
class GpuConvolutionReverb final {
  public:
    explicit GpuConvolutionReverb(GpuConvolutionReverbConfig config);
    ~GpuConvolutionReverb();

    GpuConvolutionReverb(const GpuConvolutionReverb&) = delete;
    GpuConvolutionReverb& operator=(const GpuConvolutionReverb&) = delete;

    /// Prepare the opt-in route.  The default config is deliberately disabled
    /// and fails closed until gpu_enabled is set true by the selected mode.
    bool prepare() noexcept;
    void release() noexcept;

    bool prepared() const noexcept { return prepared_; }
    bool gpu_enabled() const noexcept { return config_.gpu_enabled; }
    std::uint32_t block_size() const noexcept { return config_.block_size; }
    std::uint32_t latency_samples() const noexcept {
        return prepared_ ? 2u * config_.block_size : 0u;
    }

    // These setters are RT-safe and allocation-free.  As with the CPU
    // ZeroLatencyConvolver controls, callers change them at block boundaries.
    void set_ir_gain_db(double db) noexcept;
    void set_predelay_ms(double ms) noexcept;
    void set_wet_percent(double percent) noexcept;
    void set_dry_percent(double percent) noexcept;
    void set_width_percent(double percent) noexcept;
    void set_lowcut_hz(double hz) noexcept;
    void set_highcut_hz(double hz) noexcept;

    /// Audio-thread entry point.  It never allocates, locks, or calls a GPU
    /// API.  Each lane transport receives the same quantum and sequence shape.
    void process(const audio::BufferView<const float>& input,
                 audio::BufferView<float>& output, std::uint32_t n) noexcept;

    GpuConvolutionReverbReport report() const noexcept;

  private:
    struct Lane;

    bool valid_config() const noexcept;
    bool prepare_lanes() noexcept;
    void update_filter_coefficients() noexcept;
    void clear_runtime_buffers() noexcept;

    GpuConvolutionReverbConfig config_;
    std::array<std::unique_ptr<Lane>, 2> lanes_{};
    std::vector<float> dry_delay_;
    std::vector<float> filtered_input_[2];
    std::vector<float> predelay_ring_[2];
    std::vector<float> dry_due_[2];
    std::uint32_t predelay_capacity_ = 0;
    std::uint32_t predelay_samples_ = 0;
    std::uint32_t predelay_write_ = 0;
    std::uint32_t dry_write_ = 0;
    double ir_gain_db_ = signal::ZeroLatencyConvolver::kIrGainDbDefault;
    double ir_gain_linear_ = 1.0;
    double predelay_ms_ = signal::ZeroLatencyConvolver::kPredelayMsDefault;
    double wet_gain_ = signal::ZeroLatencyConvolver::kWetPercentDefault / 100.0;
    double dry_gain_ = signal::ZeroLatencyConvolver::kDryPercentDefault / 100.0;
    double width_ = signal::ZeroLatencyConvolver::kWidthPercentDefault / 100.0;
    double lowcut_hz_ = signal::ZeroLatencyConvolver::kLowcutHzDefault;
    double highcut_hz_ = signal::ZeroLatencyConvolver::kHighcutHzDefault;
    double hp_coef_ = 1.0;
    double lp_coef_ = 1.0;
    std::array<double, 2> hp_prev_x_{};
    std::array<double, 2> hp_z_{};
    std::array<double, 2> lp_z_{};
    bool hp_active_ = false;
    bool lp_active_ = false;
    bool prepared_ = false;
};

/// Name used by host/catalog integrations that treat the route as a node.
using GpuConvolutionReverbNode = GpuConvolutionReverb;

} // namespace pulp::gpu_audio
