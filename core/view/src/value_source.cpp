#include <pulp/view/value_source.hpp>

#include <algorithm>

#include <pulp/view/value_channel_telemetry.hpp>

namespace pulp::view {

MeterSource::~MeterSource() {
    detail::mark_meter_source_dead(telemetry_);
}

void MeterSource::publish(const MeterFrame& frame) {
    detail::PublishedValue<MeterFrame> published;
    published.value = frame;
    published.publication = ++writer_publication_;
    buffer_.write(published);
    note_publish();
}

MeterFrame MeterSource::read() {
    const auto& published = buffer_.read();
    detail::record_meter_snapshot(telemetry_, published.value,
                                  published.publication);
    return published.value;
}

VectorSource::~VectorSource() {
    detail::mark_vector_source_dead(telemetry_);
}

void VectorSource::publish(const float* samples, int count) {
    detail::PublishedValue<VectorFrame> published;
    auto& frame = published.value;
    frame.count = count < 0 ? 0 : (count > VectorFrame::kMaxSamples
                                       ? VectorFrame::kMaxSamples
                                       : count);
    if (samples != nullptr && frame.count > 0) {
        std::copy_n(samples, static_cast<std::size_t>(frame.count),
                    frame.samples.begin());
    }
    published.publication = ++writer_publication_;
    buffer_.write(published);
    note_publish();
}

VectorFrame VectorSource::read() {
    const auto& published = buffer_.read();
    detail::record_vector_snapshot(telemetry_, published.value,
                                   published.publication);
    return published.value;
}

EventSource::~EventSource() {
    detail::mark_event_source_dead(telemetry_);
}

void EventSource::publish(const ValueEvent* events, int count) {
    detail::PublishedValue<EventFrame> published;
    auto& frame = published.value;
    frame.count = events == nullptr || count < 0
                      ? 0
                      : (count > EventFrame::kMaxEvents
                             ? EventFrame::kMaxEvents
                             : count);
    if (frame.count > 0) {
        std::copy_n(events, static_cast<std::size_t>(frame.count),
                    frame.events.begin());
    }
    published.publication = ++writer_publication_;
    frame.publication = static_cast<std::uint32_t>(published.publication);
    buffer_.write(published);
    detail::push_event_telemetry(telemetry_, frame, published.publication);
    note_publish();
}

EventFrame EventSource::read() {
    return buffer_.read().value;
}

ScalarSource::~ScalarSource() {
    detail::mark_scalar_source_dead(telemetry_);
}

void ScalarSource::publish(float value) {
    detail::PublishedValue<float> published;
    published.value = value;
    published.publication = ++writer_publication_;
    buffer_.write(published);
    note_publish();
}

float ScalarSource::read() {
    const auto& published = buffer_.read();
    detail::record_scalar_snapshot(telemetry_, published.value,
                                   published.publication);
    return published.value;
}

}  // namespace pulp::view
