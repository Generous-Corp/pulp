#pragma once

/// A bounded spectral-frame stage chain hosted as one in-process graph node.
/// The graph routes its ordinary audio ports; this adapter owns the STFT/WOLA
/// boundary and invokes typed stages once per coherent channel-group frame.

#include <pulp/format/processor.hpp>
#include <pulp/signal/spectral_frame_engine.hpp>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <complex>
#include <cstdint>
#include <limits>
#include <memory>
#include <stdexcept>
#include <vector>

namespace pulp::format {

enum class SpectralFrameError : std::uint8_t {
    None,
    InvalidGeometry,
    InvalidBlock,
    InvalidFrame,
    StageFailure,
    NonFiniteFrame,
    SequenceOverflow
};

/// Borrowed only during Stage::process(). No stage may retain these pointers.
/// The bin order is DC..Nyquist, with the positive-frequency FFT convention
/// used by SpectralFrameEngine. Both endpoint bins must have zero imaginary part.
/// `origin_sample` is the signed first input sample in the analysis window;
/// negative origins describe the engine's zero-padded stream start.
struct SpectralFrameView {
    std::complex<float>* const* channels = nullptr;
    int channel_count = 0;
    int bin_count = 0;
    int fft_size = 0;
    int hop = 0;
    double sample_rate = 0;
    signal::WindowFunction::Type window = signal::WindowFunction::Type::hann;
    std::uint64_t sequence = 0;
    std::int64_t origin_sample = 0;
};

/// Stage instances are owned by the adapter. process() runs on the audio
/// thread, must be noexcept and allocation-free, and may rewrite bins in place.
class SpectralFrameStage {
  public:
    virtual ~SpectralFrameStage() = default;
    virtual bool process(const SpectralFrameView& frame) noexcept = 0;
};

/// One audio graph node containing an ordered chain of typed frame stages.
/// Add stages before prepare() only. A failed or non-finite frame is muted
/// before synthesis and the first error remains queryable until reset/prepare.
class SpectralFrameProcessor final : public Processor {
  public:
    explicit SpectralFrameProcessor(signal::SpectralFrameEngineConfig config) : config_(config) {}

    bool add_stage(std::unique_ptr<SpectralFrameStage> stage) {
        if (prepared_ || !stage || stages_.size() == kMaxStages)
            return false;
        stages_.push_back(std::move(stage));
        return true;
    }

    PluginDescriptor descriptor() const override {
        PluginDescriptor d;
        d.name = "Spectral Frame";
        d.manufacturer = "Pulp";
        d.bundle_id = "com.pulp.graph.spectral-frame";
        d.version = "1.0.0";
        d.category = PluginCategory::Effect;
        d.input_buses = {{"Main In", config_.channels, false}};
        d.output_buses = {{"Main Out", config_.channels, false}};
        return d;
    }

    void define_parameters(state::StateStore&) override {}

    void prepare(const PrepareContext& context) override {
        prepared_ = false;
        if (!std::isfinite(context.sample_rate) || context.sample_rate <= 0 ||
            config_.channels <= 0 || config_.channels > kMaxChannels ||
            context.input_channels != config_.channels ||
            context.output_channels != config_.channels || context.max_buffer_size <= 0) {
            error_.store(SpectralFrameError::InvalidGeometry, std::memory_order_relaxed);
            throw std::invalid_argument("spectral frame geometry disagrees with graph");
        }
        config_.max_block = context.max_buffer_size;
        if (!signal::checked_spectral_frame_engine_geometry<float>(config_)) {
            error_.store(SpectralFrameError::InvalidGeometry, std::memory_order_relaxed);
            throw std::invalid_argument("invalid spectral frame geometry");
        }
        engine_.prepare(config_);
        sample_rate_ = context.sample_rate;
        sequence_.store(0, std::memory_order_relaxed);
        error_.store(SpectralFrameError::None, std::memory_order_relaxed);
        prepared_ = true;
    }

    void release() override {
        prepared_ = false;
    }

    void process(audio::BufferView<float>& output, const audio::BufferView<const float>& input,
                 midi::MidiBuffer&, midi::MidiBuffer&, const ProcessContext& context) override {
        const auto count = static_cast<int>(output.num_samples());
        if (!prepared_ || count < 0 || count > config_.max_block ||
            input.num_samples() != output.num_samples() ||
            input.num_channels() != static_cast<std::size_t>(config_.channels) ||
            output.num_channels() != static_cast<std::size_t>(config_.channels)) {
            output.clear();
            set_error_(SpectralFrameError::InvalidBlock);
            return;
        }
        if (context.reset_requested)
            reset();
        std::array<const float*, kMaxChannels> in{};
        std::array<float*, kMaxChannels> out{};
        for (int ch = 0; ch < config_.channels; ++ch) {
            in[ch] = input.channel_ptr(static_cast<std::size_t>(ch));
            out[ch] = output.channel_ptr(static_cast<std::size_t>(ch));
        }
        engine_.process(
            in.data(), out.data(), count,
            [this](std::complex<float>* const* bins, int bin_count) noexcept {
                const auto sequence = sequence_.load(std::memory_order_relaxed);
                if (sequence > static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max() /
                                                          config_.analysis_hop)) {
                    set_error_(SpectralFrameError::SequenceOverflow);
                    for (int ch = 0; ch < config_.channels; ++ch)
                        std::fill_n(bins[ch], bin_count, std::complex<float>{});
                    return;
                }
                const auto origin = engine_.first_frame_start() +
                                    static_cast<std::int64_t>(sequence) * config_.analysis_hop;
                SpectralFrameView view{bins,
                                       config_.channels,
                                       bin_count,
                                       config_.fft_size,
                                       config_.analysis_hop,
                                       sample_rate_,
                                       config_.window,
                                       sequence,
                                       origin};
                sequence_.store(sequence + 1, std::memory_order_relaxed);
                if (!finite_(view)) {
                    set_error_(SpectralFrameError::InvalidFrame);
                    mute_(view);
                    return;
                }
                for (const auto& stage : stages_) {
                    if (!stage->process(view)) {
                        set_error_(SpectralFrameError::StageFailure);
                        mute_(view);
                        return;
                    }
                    if (!finite_(view)) {
                        set_error_(SpectralFrameError::NonFiniteFrame);
                        mute_(view);
                        return;
                    }
                }
            });
    }

    int latency_samples() const override {
        return prepared_ ? engine_.latency_samples() : 0;
    }
    SpectralFrameError error() const noexcept {
        return error_.load(std::memory_order_relaxed);
    }
    std::uint64_t frames_processed() const noexcept {
        return sequence_.load(std::memory_order_relaxed);
    }
    void reset() noexcept {
        if (prepared_)
            engine_.reset();
        sequence_.store(0, std::memory_order_relaxed);
        error_.store(SpectralFrameError::None, std::memory_order_relaxed);
    }

  private:
    static constexpr std::size_t kMaxStages = 8;
    static constexpr int kMaxChannels = 16;

    static bool finite_(const SpectralFrameView& frame) noexcept {
        if (frame.channels == nullptr || frame.channel_count <= 0 ||
            frame.channel_count > kMaxChannels || frame.bin_count != frame.fft_size / 2 + 1)
            return false;
        for (int ch = 0; ch < frame.channel_count; ++ch) {
            if (frame.channels[ch] == nullptr)
                return false;
            for (int bin = 0; bin < frame.bin_count; ++bin) {
                const auto value = frame.channels[ch][bin];
                if (!std::isfinite(value.real()) || !std::isfinite(value.imag()))
                    return false;
            }
            if (frame.channels[ch][0].imag() != 0 ||
                frame.channels[ch][frame.bin_count - 1].imag() != 0)
                return false;
        }
        return true;
    }

    static void mute_(const SpectralFrameView& frame) noexcept {
        for (int ch = 0; ch < frame.channel_count; ++ch)
            std::fill_n(frame.channels[ch], frame.bin_count, std::complex<float>{});
    }
    void set_error_(SpectralFrameError error) noexcept {
        auto expected = SpectralFrameError::None;
        error_.compare_exchange_strong(expected, error, std::memory_order_relaxed);
    }

    signal::SpectralFrameEngineConfig config_;
    signal::SpectralFrameEngineT<float> engine_;
    std::vector<std::unique_ptr<SpectralFrameStage>> stages_;
    double sample_rate_ = 0;
    std::atomic<std::uint64_t> sequence_{0};
    std::atomic<SpectralFrameError> error_{SpectralFrameError::None};
    bool prepared_ = false;
};

} // namespace pulp::format
