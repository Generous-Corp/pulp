#include <pulp/vellum_audio/audio_display_provider.hpp>

#include <algorithm>

namespace pulp::vellum_audio {

AudioDisplayProvider::AudioDisplayProvider(std::vector<ChannelSpec> channels) {
    for (auto& spec : channels) {
        channels_.emplace_back();
        auto& channel = channels_.back();
        channel.name = std::move(spec.name);
        channel.unit = std::move(spec.unit);
        channel.shape = spec.shape;
        channel.neutral = spec.neutral;
    }
}

AudioDisplayProvider::Channel* AudioDisplayProvider::at(std::size_t index) noexcept {
    return index < channels_.size() ? &channels_[index] : nullptr;
}

const AudioDisplayProvider::Channel* AudioDisplayProvider::at(std::size_t index) const noexcept {
    return index < channels_.size() ? &channels_[index] : nullptr;
}

void AudioDisplayProvider::publish_meter(std::size_t index, std::span<const float> rms,
                                         std::span<const float> peak) noexcept {
    publish_meter(index, rms, peak, {});
}

void AudioDisplayProvider::publish_meter(std::size_t index, std::span<const float> rms,
                                         std::span<const float> peak,
                                         AudioFrameInfo info) noexcept {
    auto* c = at(index);
    if (c == nullptr || c->shape != ::vellum::value::ChannelShape::meter)
        return;
    c->meter.write_with([&](MeterFrame& frame) noexcept {
        const auto n = std::min({rms.size(), peak.size(), kMaxMeterChannels});
        frame.count = static_cast<std::uint32_t>(n);
        std::copy_n(rms.begin(), n, frame.rms.begin());
        std::copy_n(peak.begin(), n, frame.peak.begin());
        info.sequence =
            info.sequence == 0 ? c->published.load(std::memory_order_relaxed) + 1 : info.sequence;
        frame.sequence = info.sequence;
        frame.info = info;
    });
    c->published.fetch_add(1, std::memory_order_relaxed);
}

void AudioDisplayProvider::publish_scope(std::size_t index,
                                         std::span<const float> samples) noexcept {
    publish_scope(index, samples, {});
}

void AudioDisplayProvider::publish_scope(std::size_t index, std::span<const float> samples,
                                         AudioFrameInfo info) noexcept {
    auto* c = at(index);
    if (c == nullptr || c->shape != ::vellum::value::ChannelShape::vector)
        return;
    c->scope.write_with([&](ScopeFrame& frame) noexcept {
        const auto n = std::min(samples.size(), kMaxScopeSamples);
        frame.count = static_cast<std::uint32_t>(n);
        std::copy_n(samples.begin(), n, frame.samples.begin());
        info.sequence =
            info.sequence == 0 ? c->published.load(std::memory_order_relaxed) + 1 : info.sequence;
        frame.sequence = info.sequence;
        frame.info = info;
    });
    c->published.fetch_add(1, std::memory_order_relaxed);
}

bool AudioDisplayProvider::read_info(std::size_t index, AudioFrameInfo& info) noexcept {
    auto* c = at(index);
    if (c == nullptr || c->published.load(std::memory_order_acquire) == 0)
        return false;
    if (c->shape == ::vellum::value::ChannelShape::meter)
        info = c->meter.read().info;
    else if (c->shape == ::vellum::value::ChannelShape::vector)
        info = c->scope.read().info;
    else
        return false;
    return true;
}

std::size_t AudioDisplayProvider::channel_count() const noexcept {
    return channels_.size();
}

::vellum::value::ChannelInfo AudioDisplayProvider::channel(std::size_t index) const noexcept {
    const auto* c = at(index);
    return c == nullptr ? ::vellum::value::ChannelInfo{}
                        : ::vellum::value::ChannelInfo{c->name, c->unit, c->shape, c->neutral};
}

bool AudioDisplayProvider::read_scalar(std::size_t, float&) noexcept {
    return false;
}

std::size_t AudioDisplayProvider::read_meter(std::size_t index, std::span<float> rms,
                                             std::span<float> peak) noexcept {
    auto* c = at(index);
    if (c == nullptr || c->shape != ::vellum::value::ChannelShape::meter)
        return 0;
    const auto& frame = c->meter.read();
    const auto n = std::min<std::size_t>(frame.count, kMaxMeterChannels);
    const auto copied = std::min({n, rms.size(), peak.size()});
    std::copy_n(frame.rms.begin(), copied, rms.begin());
    std::copy_n(frame.peak.begin(), copied, peak.begin());
    return copied;
}

std::size_t AudioDisplayProvider::read_vector(std::size_t index,
                                              std::span<float> samples) noexcept {
    auto* c = at(index);
    if (c == nullptr || c->shape != ::vellum::value::ChannelShape::vector)
        return 0;
    const auto& frame = c->scope.read();
    const auto n = std::min<std::size_t>(frame.count, std::min(samples.size(), kMaxScopeSamples));
    std::copy_n(frame.samples.begin(), n, samples.begin());
    return n;
}

std::size_t AudioDisplayProvider::read_events(std::size_t,
                                              std::span<::vellum::value::ChannelEvent>) noexcept {
    return 0;
}

std::uint32_t AudioDisplayProvider::publish_count(std::size_t index) const noexcept {
    const auto* c = at(index);
    if (c == nullptr)
        return 0;
    return c->published.load(std::memory_order_relaxed);
}

} // namespace pulp::vellum_audio
