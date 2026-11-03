#include <pulp/format/value_channel_visual_port.hpp>

#include <pulp/dataplane/value_channel_set.hpp>
#include <pulp/format/processor.hpp>

#include <algorithm>

namespace pulp::format {

namespace {

VisualChannelShape to_visual_shape(dataplane::ValueChannelShape shape) noexcept {
    switch (shape) {
    case dataplane::ValueChannelShape::scalar:
        return VisualChannelShape::scalar;
    case dataplane::ValueChannelShape::meter:
        return VisualChannelShape::meter;
    case dataplane::ValueChannelShape::vector:
        return VisualChannelShape::vector;
    case dataplane::ValueChannelShape::events:
        return VisualChannelShape::events;
    }
    return VisualChannelShape::scalar;
}

/// A frame's own count, bounded by its capacity: publish() stores frames
/// verbatim, so a count is never trusted to gate an array access.
std::size_t bounded(int count, int capacity) noexcept {
    return static_cast<std::size_t>(std::clamp(count, 0, capacity));
}

} // namespace

ValueChannelVisualPort::ValueChannelVisualPort(const dataplane::ValueChannelSet* channels) {
    if (channels == nullptr)
        return;
    const auto& infos = channels->infos();
    channels_.reserve(infos.size());
    for (const auto& info : infos) {
        Channel c;
        c.info.name = info.name;
        c.info.unit = info.unit;
        c.info.shape = to_visual_shape(info.shape);
        c.info.neutral = info.neutral;
        switch (info.shape) {
        case dataplane::ValueChannelShape::scalar:
            c.scalar = channels->scalar(info.name);
            break;
        case dataplane::ValueChannelShape::meter:
            c.meter = channels->meter(info.name);
            break;
        case dataplane::ValueChannelShape::vector:
            c.vector = channels->vector(info.name);
            break;
        case dataplane::ValueChannelShape::events:
            c.events = channels->events(info.name);
            break;
        }
        channels_.push_back(c);
    }
}

ValueChannelVisualPort::ValueChannelVisualPort(Processor& processor)
    : ValueChannelVisualPort(processor.value_channels()) {}

const ValueChannelVisualPort::Channel*
ValueChannelVisualPort::at(std::size_t index) const noexcept {
    return index < channels_.size() ? &channels_[index] : nullptr;
}

std::size_t ValueChannelVisualPort::channel_count() const noexcept { return channels_.size(); }

VisualChannelInfo ValueChannelVisualPort::channel(std::size_t index) const noexcept {
    const Channel* c = at(index);
    return c != nullptr ? c->info : VisualChannelInfo{};
}

bool ValueChannelVisualPort::read_scalar(std::size_t index, float& value) noexcept {
    const Channel* c = at(index);
    if (c == nullptr || c->scalar == nullptr || c->scalar->publish_seq() == 0)
        return false;
    value = c->scalar->read();
    return true;
}

std::size_t ValueChannelVisualPort::read_meter(std::size_t index, std::span<float> rms,
                                               std::span<float> peak) noexcept {
    const Channel* c = at(index);
    if (c == nullptr || c->meter == nullptr)
        return 0;
    const dataplane::MeterFrame frame = c->meter->read();
    const std::size_t carried = bounded(frame.channels, dataplane::MeterFrame::kMaxChannels);
    std::copy_n(frame.rms.begin(), std::min(carried, rms.size()), rms.begin());
    std::copy_n(frame.peak.begin(), std::min(carried, peak.size()), peak.begin());
    return carried;
}

std::size_t ValueChannelVisualPort::read_vector(std::size_t index,
                                                std::span<float> samples) noexcept {
    const Channel* c = at(index);
    if (c == nullptr || c->vector == nullptr)
        return 0;
    const dataplane::VectorFrame frame = c->vector->read();
    const std::size_t n =
        std::min(bounded(frame.count, dataplane::VectorFrame::kMaxSamples), samples.size());
    std::copy_n(frame.samples.begin(), n, samples.begin());
    return n;
}

std::size_t ValueChannelVisualPort::read_events(std::size_t index,
                                                std::span<VisualEvent> events) noexcept {
    const Channel* c = at(index);
    if (c == nullptr || c->events == nullptr)
        return 0;
    const dataplane::EventFrame frame = c->events->read();
    const std::size_t n =
        std::min(bounded(frame.count, dataplane::EventFrame::kMaxEvents), events.size());
    for (std::size_t i = 0; i < n; ++i)
        events[i] = VisualEvent{frame.events[i].frame_index, frame.events[i].value};
    return n;
}

std::uint32_t ValueChannelVisualPort::publish_count(std::size_t index) const noexcept {
    const Channel* c = at(index);
    if (c == nullptr)
        return 0;
    if (c->scalar != nullptr)
        return c->scalar->publish_seq();
    if (c->meter != nullptr)
        return c->meter->publish_seq();
    if (c->vector != nullptr)
        return c->vector->publish_seq();
    if (c->events != nullptr)
        return c->events->publish_seq();
    return 0;
}

} // namespace pulp::format
