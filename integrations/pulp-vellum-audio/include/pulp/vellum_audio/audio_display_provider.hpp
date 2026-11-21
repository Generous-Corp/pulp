#pragma once

/// @file
/// A fixed-channel audio display feed for Vellum widgets.
///
/// The audio thread publishes complete meter or scope frames into a
/// `TripleBuffer`; the editor thread reads the latest complete frame. The
/// channel list and storage are allocated when the provider is made, so the
/// publish path is allocation-free and never waits for the editor.

#include <pulp/runtime/triple_buffer.hpp>

#include <vellum/value/value_feed.hpp>

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <span>
#include <string>
#include <vector>

namespace vellum::view {
class View;
}

namespace pulp::vellum_audio {

struct AudioFrameInfo {
    float viewport_min_hz = 20.0f;
    float viewport_max_hz = 20000.0f;
    float floor_db = -120.0f;
    double sample_rate = 48000.0;
    std::uint64_t epoch = 0;
    std::uint32_t sequence = 0;
    std::uint32_t dropped_frames = 0;
};

class AudioDisplayProvider final : public ::vellum::value::ValueFeed {
  public:
    static constexpr std::size_t kMaxMeterChannels = 32;
    static constexpr std::size_t kMaxScopeSamples = 4096;

    struct ChannelSpec {
        std::string name;
        std::string unit;
        ::vellum::value::ChannelShape shape = ::vellum::value::ChannelShape::scalar;
        float neutral = 0.0f;
    };

    explicit AudioDisplayProvider(std::vector<ChannelSpec> channels);
    ~AudioDisplayProvider() override = default;

    AudioDisplayProvider(const AudioDisplayProvider&) = delete;
    AudioDisplayProvider& operator=(const AudioDisplayProvider&) = delete;

    /// Audio-thread publication. `index` is resolved once from `find()`.
    void publish_meter(std::size_t index, std::span<const float> rms,
                       std::span<const float> peak) noexcept;
    void publish_meter(std::size_t index, std::span<const float> rms, std::span<const float> peak,
                       AudioFrameInfo info) noexcept;
    /// Audio-thread publication. Samples beyond kMaxScopeSamples are dropped.
    void publish_scope(std::size_t index, std::span<const float> samples) noexcept;
    void publish_scope(std::size_t index, std::span<const float> samples,
                       AudioFrameInfo info) noexcept;

    /// Reads the latest frame metadata. False means the channel has never been published.
    bool read_info(std::size_t index, AudioFrameInfo& info) noexcept;

    [[nodiscard]] std::size_t channel_count() const noexcept override;
    [[nodiscard]] ::vellum::value::ChannelInfo channel(std::size_t index) const noexcept override;
    bool read_scalar(std::size_t index, float& value) noexcept override;
    std::size_t read_meter(std::size_t index, std::span<float> rms,
                           std::span<float> peak) noexcept override;
    std::size_t read_vector(std::size_t index, std::span<float> samples) noexcept override;
    /// Like read_meter / read_vector, but also copies the frame's metadata from
    /// the same TripleBuffer read, so values and info always describe one frame.
    /// `info` is left untouched only for an unknown index or a wrong shape.
    std::size_t read_meter(std::size_t index, std::span<float> rms, std::span<float> peak,
                           AudioFrameInfo& info) noexcept;
    std::size_t read_vector(std::size_t index, std::span<float> samples,
                            AudioFrameInfo& info) noexcept;
    std::size_t read_events(std::size_t index,
                            std::span<::vellum::value::ChannelEvent> events) noexcept override;
    [[nodiscard]] std::uint32_t publish_count(std::size_t index) const noexcept override;

  private:
    struct MeterFrame {
        std::array<float, kMaxMeterChannels> rms{};
        std::array<float, kMaxMeterChannels> peak{};
        std::uint32_t count = 0;
        std::uint32_t sequence = 0;
        AudioFrameInfo info;
    };
    struct ScopeFrame {
        std::array<float, kMaxScopeSamples> samples{};
        std::uint32_t count = 0;
        std::uint32_t sequence = 0;
        AudioFrameInfo info;
    };
    struct Channel {
        std::string name;
        std::string unit;
        ::vellum::value::ChannelShape shape = ::vellum::value::ChannelShape::scalar;
        float neutral = 0.0f;
        mutable runtime::TripleBuffer<MeterFrame> meter;
        mutable runtime::TripleBuffer<ScopeFrame> scope;
        std::atomic<std::uint32_t> published{0};
    };

    [[nodiscard]] Channel* at(std::size_t index) noexcept;
    [[nodiscard]] const Channel* at(std::size_t index) const noexcept;
    std::deque<Channel> channels_;
};

} // namespace pulp::vellum_audio

namespace pulp::vellum_audio {

/// Register the Vellum data displays used by scripted `createMeter` and
/// `createSpectrum` calls. The feed is borrowed by the factories and must
/// outlive the scripted editor session that uses them.
void register_audio_widgets(::vellum::value::ValueFeed* feed);

/// Sample every registered audio display below a scripted editor root once per
/// editor idle tick.
void sample_audio_widgets(::vellum::view::View& root);

} // namespace pulp::vellum_audio
