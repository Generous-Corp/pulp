#include <pulp/audio/realtime_time_stretch.hpp>

#include <pulp/signal/realtime_pitch_time_processor.hpp>

#include <cmath>
#include <limits>
#include <utility>

namespace pulp::audio {
namespace {

signal::RealtimePitchTimeConfig
signal_config(const RealtimeTimeStretchConfig& config) noexcept {
    signal::RealtimePitchTimeConfig result;
    result.mode = signal::PitchTimeMode::time_stretch;
    result.quality = config.quality == RealtimeTimeStretchQuality::quality
                         ? signal::PitchTimeQuality::quality
                         : signal::PitchTimeQuality::low_latency;
    result.channels = config.channels;
    result.max_block = config.max_block;
    result.max_time_ratio = config.max_time_ratio;
    result.fft_size = config.fft_size;
    result.analysis_hop = config.analysis_hop;
    return result;
}

RealtimeTimeStretchPrepareStatus
prepare_status(signal::PitchTimePrepareStatus status) noexcept {
    using Source = signal::PitchTimePrepareStatus;
    switch (status) {
    case Source::prepared:
        return RealtimeTimeStretchPrepareStatus::prepared;
    case Source::invalid_sample_rate:
        return RealtimeTimeStretchPrepareStatus::invalid_sample_rate;
    case Source::invalid_channel_count:
        return RealtimeTimeStretchPrepareStatus::invalid_channel_count;
    case Source::invalid_max_block:
        return RealtimeTimeStretchPrepareStatus::invalid_max_block;
    case Source::invalid_max_time_ratio:
        return RealtimeTimeStretchPrepareStatus::invalid_max_time_ratio;
    case Source::invalid_spectral_geometry:
        return RealtimeTimeStretchPrepareStatus::invalid_spectral_geometry;
    case Source::invalid_max_pitch_semitones:
    case Source::unrepresentable_capacity:
        return RealtimeTimeStretchPrepareStatus::unrepresentable_capacity;
    }
    return RealtimeTimeStretchPrepareStatus::unrepresentable_capacity;
}

RealtimeTimeStretchStreamFeedStatus
feed_status(signal::PitchTimeStreamFeedStatus status) noexcept {
    using Source = signal::PitchTimeStreamFeedStatus;
    switch (status) {
    case Source::accepted:
        return RealtimeTimeStretchStreamFeedStatus::accepted;
    case Source::backpressure:
        return RealtimeTimeStretchStreamFeedStatus::backpressure;
    case Source::input_closed:
        return RealtimeTimeStretchStreamFeedStatus::input_closed;
    case Source::invalid_request:
        return RealtimeTimeStretchStreamFeedStatus::invalid_request;
    }
    return RealtimeTimeStretchStreamFeedStatus::invalid_request;
}

RealtimeTimeStretchStreamFinalizeStatus
finalize_status(signal::PitchTimeStreamFinalizeStatus status) noexcept {
    using Source = signal::PitchTimeStreamFinalizeStatus;
    switch (status) {
    case Source::draining:
        return RealtimeTimeStretchStreamFinalizeStatus::draining;
    case Source::backpressure:
        return RealtimeTimeStretchStreamFinalizeStatus::backpressure;
    case Source::complete:
        return RealtimeTimeStretchStreamFinalizeStatus::complete;
    case Source::invalid_mode:
    case Source::invalid_request:
        return RealtimeTimeStretchStreamFinalizeStatus::invalid_request;
    }
    return RealtimeTimeStretchStreamFinalizeStatus::invalid_request;
}

RealtimeTimeStretchStreamFinalizePlan
finalize_plan(signal::PitchTimeStreamFinalizePlan plan) noexcept {
    using Source = signal::PitchTimeStreamFinalizePlanStatus;
    auto status = RealtimeTimeStretchStreamFinalizePlanStatus::invalid_request;
    switch (plan.status) {
    case Source::ready:
        status = RealtimeTimeStretchStreamFinalizePlanStatus::ready;
        break;
    case Source::needs_drain:
        status = RealtimeTimeStretchStreamFinalizePlanStatus::needs_drain;
        break;
    case Source::complete:
        status = RealtimeTimeStretchStreamFinalizePlanStatus::complete;
        break;
    case Source::invalid_mode:
    case Source::invalid_request:
        status = RealtimeTimeStretchStreamFinalizePlanStatus::invalid_request;
        break;
    }
    return {status, plan.samples};
}

} // namespace

struct RealtimeTimeStretchProcessor::Impl {
    signal::RealtimePitchTimeProcessor processor;
};

static_assert(kRealtimeTimeStretchMaximumChannels ==
              signal::kRealtimePitchTimeMaximumChannels);

RealtimeTimeStretchPrepareStatus checked_realtime_time_stretch_prepared_geometry(
    const RealtimeTimeStretchConfig& config, std::uint64_t requested_max_bytes,
    RealtimeTimeStretchPreparedGeometry& prepared) noexcept {
    if (config.channels < 1 || config.channels > kRealtimeTimeStretchMaximumChannels)
        return RealtimeTimeStretchPrepareStatus::invalid_channel_count;
    if (config.max_block <= 0)
        return RealtimeTimeStretchPrepareStatus::invalid_max_block;

    signal::RealtimePitchTimePreparedGeometry<float> geometry;
    const auto status = signal::checked_realtime_pitch_time_prepared_geometry(
        signal_config(config), 1.0, requested_max_bytes, geometry);
    if (status != signal::PitchTimePrepareStatus::prepared)
        return prepare_status(status);
    if (requested_max_bytes < sizeof(RealtimeTimeStretchProcessor::Impl))
        return RealtimeTimeStretchPrepareStatus::unrepresentable_capacity;
    if (geometry.retained_bytes >
        std::numeric_limits<std::uint64_t>::max() -
            sizeof(RealtimeTimeStretchProcessor::Impl))
        return RealtimeTimeStretchPrepareStatus::unrepresentable_capacity;

    prepared = {
        geometry.maximum_stream_output_lag_samples,
        geometry.retained_bytes + sizeof(RealtimeTimeStretchProcessor::Impl),
    };
    return RealtimeTimeStretchPrepareStatus::prepared;
}

RealtimeTimeStretchProcessor::RealtimeTimeStretchProcessor() noexcept = default;
RealtimeTimeStretchProcessor::~RealtimeTimeStretchProcessor() = default;
RealtimeTimeStretchProcessor::RealtimeTimeStretchProcessor(
    RealtimeTimeStretchProcessor&&) noexcept = default;
RealtimeTimeStretchProcessor&
RealtimeTimeStretchProcessor::operator=(RealtimeTimeStretchProcessor&&) noexcept = default;

RealtimeTimeStretchPrepareStatus RealtimeTimeStretchProcessor::prepare(
    double sample_rate, const RealtimeTimeStretchConfig& config,
    std::uint64_t requested_max_bytes) {
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0)
        return RealtimeTimeStretchPrepareStatus::invalid_sample_rate;
    RealtimeTimeStretchPreparedGeometry geometry;
    const auto admission = checked_realtime_time_stretch_prepared_geometry(
        config, requested_max_bytes, geometry);
    if (admission != RealtimeTimeStretchPrepareStatus::prepared)
        return admission;

    auto candidate = std::make_unique<Impl>();
    const auto status = candidate->processor.prepare(
        sample_rate, signal_config(config), requested_max_bytes);
    if (status != signal::PitchTimePrepareStatus::prepared)
        return prepare_status(status);
    impl_ = std::move(candidate);
    return RealtimeTimeStretchPrepareStatus::prepared;
}

void RealtimeTimeStretchProcessor::reset() noexcept {
    if (impl_)
        impl_->processor.reset();
}

void RealtimeTimeStretchProcessor::set_time_ratio(float ratio) noexcept {
    if (impl_)
        impl_->processor.set_time_ratio(ratio);
}

RealtimeTimeStretchStreamFeedStatus
RealtimeTimeStretchProcessor::feed(const float* const* input, int num_samples) noexcept {
    return impl_ ? feed_status(impl_->processor.feed(input, num_samples))
                 : RealtimeTimeStretchStreamFeedStatus::invalid_request;
}

int RealtimeTimeStretchProcessor::available_stretched() const noexcept {
    return impl_ ? impl_->processor.available_stretched() : 0;
}

int RealtimeTimeStretchProcessor::output_free_space() const noexcept {
    return impl_ ? impl_->processor.output_free_space() : 0;
}

int RealtimeTimeStretchProcessor::samples_until_next_analysis_frame() const noexcept {
    return impl_ ? impl_->processor.samples_until_next_analysis_frame() : 0;
}

RealtimeTimeStretchStreamFinalizePlan
RealtimeTimeStretchProcessor::plan_finalize(int max_samples) const noexcept {
    return impl_ ? finalize_plan(impl_->processor.plan_finalize(max_samples))
                 : RealtimeTimeStretchStreamFinalizePlan{};
}

RealtimeTimeStretchStreamFinalizeStatus
RealtimeTimeStretchProcessor::finalize(int max_samples) noexcept {
    return impl_ ? finalize_status(impl_->processor.finalize(max_samples))
                 : RealtimeTimeStretchStreamFinalizeStatus::invalid_request;
}

int RealtimeTimeStretchProcessor::read_stretched(float* const* output,
                                                int num_samples) noexcept {
    return impl_ ? impl_->processor.read_stretched(output, num_samples) : 0;
}

} // namespace pulp::audio
