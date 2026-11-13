#include <pulp/vellum/visual_port_feed.hpp>

#include <algorithm>
#include <array>

namespace pulp::vellum {

namespace {

using ::vellum::value::ChannelShape;

ChannelShape shape_of(format::VisualChannelShape shape) noexcept {
    switch (shape) {
    case format::VisualChannelShape::scalar:
        return ChannelShape::scalar;
    case format::VisualChannelShape::meter:
        return ChannelShape::meter;
    case format::VisualChannelShape::vector:
        return ChannelShape::vector;
    case format::VisualChannelShape::events:
        return ChannelShape::events;
    }
    return ChannelShape::scalar;
}

// The most occurrences one read copies. The port's events channels are
// bounded lists from a single audio block; a block never carries this many.
constexpr std::size_t kMaxEventsPerRead = 256;

} // namespace

std::size_t VisualPortFeed::channel_count() const noexcept {
    return port_.channel_count();
}

::vellum::value::ChannelInfo VisualPortFeed::channel(std::size_t index) const noexcept {
    if (index >= port_.channel_count())
        return {};
    const auto c = port_.channel(index);
    return {c.name, c.unit, shape_of(c.shape), c.neutral};
}

bool VisualPortFeed::read_scalar(std::size_t index, float& value) noexcept {
    return index < port_.channel_count() && port_.read_scalar(index, value);
}

std::size_t VisualPortFeed::read_meter(std::size_t index, std::span<float> rms,
                                       std::span<float> peak) noexcept {
    return index < port_.channel_count() ? port_.read_meter(index, rms, peak) : 0;
}

std::size_t VisualPortFeed::read_vector(std::size_t index, std::span<float> samples) noexcept {
    return index < port_.channel_count() ? port_.read_vector(index, samples) : 0;
}

std::size_t VisualPortFeed::read_events(std::size_t index,
                                        std::span<::vellum::value::ChannelEvent> events) noexcept {
    if (index >= port_.channel_count())
        return 0;
    std::array<format::VisualEvent, kMaxEventsPerRead> block;
    const std::size_t want = std::min(events.size(), block.size());
    const std::size_t got = std::min(port_.read_events(index, std::span(block.data(), want)), want);
    for (std::size_t i = 0; i < got; ++i)
        events[i] = {block[i].frame_index, block[i].value};
    return got;
}

std::uint32_t VisualPortFeed::publish_count(std::size_t index) const noexcept {
    return index < port_.channel_count() ? port_.publish_count(index) : 0;
}

} // namespace pulp::vellum
