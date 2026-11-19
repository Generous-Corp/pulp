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

namespace pulp::vellum_audio {

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
    /// Audio-thread publication. Samples beyond kMaxScopeSamples are dropped.
    void publish_scope(std::size_t index, std::span<const float> samples) noexcept;

    [[nodiscard]] std::size_t channel_count() const noexcept override;
    [[nodiscard]] ::vellum::value::ChannelInfo channel(std::size_t index) const noexcept override;
    bool read_scalar(std::size_t index, float& value) noexcept override;
    std::size_t read_meter(std::size_t index, std::span<float> rms,
                           std::span<float> peak) noexcept override;
    std::size_t read_vector(std::size_t index, std::span<float> samples) noexcept override;
    std::size_t read_events(std::size_t index,
                            std::span<::vellum::value::ChannelEvent> events) noexcept override;
    [[nodiscard]] std::uint32_t publish_count(std::size_t index) const noexcept override;

  private:
    struct MeterFrame {
        std::array<float, kMaxMeterChannels> rms{};
        std::array<float, kMaxMeterChannels> peak{};
        std::uint32_t count = 0;
        std::uint32_t sequence = 0;
    };
    struct ScopeFrame {
        std::array<float, kMaxScopeSamples> samples{};
        std::uint32_t count = 0;
        std::uint32_t sequence = 0;
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
