#include <pulp/format/offline_sample_bounce.hpp>

#include <pulp/audio/sample_asset_io.hpp>

#include <algorithm>
#include <cmath>
#include <limits>

namespace pulp::format {
namespace {

bool sample_rate_to_uint32(double sample_rate, std::uint32_t& out) noexcept {
    if (!std::isfinite(sample_rate) || sample_rate <= 0.0) return false;
    if (sample_rate > static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return false;
    }

    const auto rounded = std::round(sample_rate);
    if (std::abs(sample_rate - rounded) > 0.000001) return false;

    out = static_cast<std::uint32_t>(rounded);
    return true;
}

bool sample_rate_allowed(const std::vector<std::uint32_t>& allowed,
                         std::uint32_t sample_rate) {
    if (allowed.empty()) return true;
    return std::find(allowed.begin(), allowed.end(), sample_rate) != allowed.end();
}

OfflineSampleBounceStatus validate_audio_shape(
    std::uint64_t channels_raw,
    std::uint64_t frames,
    double sample_rate,
    const OfflineSampleBouncePolicy& policy,
    std::uint64_t& decoded_bytes) noexcept {
    decoded_bytes = 0;

    if (channels_raw == 0 || frames == 0) {
        return OfflineSampleBounceStatus::empty_render;
    }
    if (channels_raw > std::numeric_limits<std::uint32_t>::max()) {
        return OfflineSampleBounceStatus::channel_budget_exceeded;
    }

    std::uint32_t integer_sample_rate = 0;
    if (!sample_rate_to_uint32(sample_rate, integer_sample_rate)) {
        return OfflineSampleBounceStatus::invalid_sample_rate;
    }
    if (!sample_rate_allowed(policy.allowed_sample_rates, integer_sample_rate)) {
        return OfflineSampleBounceStatus::sample_rate_not_allowed;
    }

    const auto channels = static_cast<std::uint32_t>(channels_raw);
    if (policy.max_channels > 0 && channels > policy.max_channels) {
        return OfflineSampleBounceStatus::channel_budget_exceeded;
    }
    if (policy.max_frames > 0 && frames > policy.max_frames) {
        return OfflineSampleBounceStatus::frame_budget_exceeded;
    }

    decoded_bytes = audio::sample_asset_decoded_bytes(channels, frames);
    if (decoded_bytes == 0) {
        return OfflineSampleBounceStatus::byte_budget_exceeded;
    }
    if (policy.max_decoded_bytes > 0 && decoded_bytes > policy.max_decoded_bytes) {
        return OfflineSampleBounceStatus::byte_budget_exceeded;
    }
    return OfflineSampleBounceStatus::ok;
}

OfflineSampleBounceStatus validate_rendered_audio(
    const audio::Buffer<float>& audio,
    double sample_rate,
    const OfflineSampleBouncePolicy& policy,
    std::uint64_t& decoded_bytes) noexcept {
    return validate_audio_shape(static_cast<std::uint64_t>(audio.num_channels()),
                                static_cast<std::uint64_t>(audio.num_samples()),
                                sample_rate,
                                policy,
                                decoded_bytes);
}

OfflineSampleBounceStatus preflight_requested_audio(
    const OfflineRenderConfig& config,
    const OfflineSampleBounceRequest& request,
    std::uint64_t& decoded_bytes) noexcept {
    return validate_audio_shape(config.output_channels,
                                request.render_options.frame_count,
                                config.sample_rate,
                                request.policy,
                                decoded_bytes);
}

} // namespace

const char* offline_sample_bounce_status_name(
    OfflineSampleBounceStatus status) noexcept {
    switch (status) {
        case OfflineSampleBounceStatus::ok: return "ok";
        case OfflineSampleBounceStatus::render_failed: return "render_failed";
        case OfflineSampleBounceStatus::empty_render: return "empty_render";
        case OfflineSampleBounceStatus::invalid_sample_rate: return "invalid_sample_rate";
        case OfflineSampleBounceStatus::channel_budget_exceeded: return "channel_budget_exceeded";
        case OfflineSampleBounceStatus::frame_budget_exceeded: return "frame_budget_exceeded";
        case OfflineSampleBounceStatus::byte_budget_exceeded: return "byte_budget_exceeded";
        case OfflineSampleBounceStatus::sample_rate_not_allowed: return "sample_rate_not_allowed";
        case OfflineSampleBounceStatus::bank_unprepared: return "bank_unprepared";
        case OfflineSampleBounceStatus::publish_failed: return "publish_failed";
    }
    return "unknown";
}

OfflineSampleBounceResult OfflineSampleBounce::render_to_buffer(
    OfflineRenderHost& host,
    const OfflineSampleBounceRequest& request) {
    OfflineSampleBounceResult result;
    if (!host.prepared()) {
        result.status = OfflineSampleBounceStatus::render_failed;
        return result;
    }

    result.sample_rate = host.config().sample_rate;
    result.status = preflight_requested_audio(host.config(), request, result.decoded_bytes);
    if (!result.ok()) return result;

    result.render = host.render(request.render_options);
    if (!result.render.ok) {
        result.status = OfflineSampleBounceStatus::render_failed;
        result.decoded_bytes = 0;
        return result;
    }

    result.status = validate_rendered_audio(result.render.audio,
                                            result.sample_rate,
                                            request.policy,
                                            result.decoded_bytes);
    return result;
}

} // namespace pulp::format
