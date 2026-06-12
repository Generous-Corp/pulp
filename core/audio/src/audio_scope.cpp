#include <pulp/audio/audio_scope.hpp>

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <limits>
#include <numeric>
#include <utility>

namespace pulp::audio {
namespace {

void add_warning(std::vector<std::string>& warnings, std::string warning) {
    warnings.push_back(std::move(warning));
}

bool is_rising_zero(float previous, float current) {
    return previous < 0.0f && current >= 0.0f;
}

std::uint32_t clamp_u32(std::size_t value) {
    constexpr auto max = static_cast<std::size_t>(
        std::numeric_limits<std::uint32_t>::max());
    return static_cast<std::uint32_t>(std::min(value, max));
}

}  // namespace

std::string_view audio_scope_trigger_mode_name(AudioScopeTriggerMode mode) {
    switch (mode) {
        case AudioScopeTriggerMode::kNone:       return "none";
        case AudioScopeTriggerMode::kRisingZero: return "rising_zero";
    }
    return "unknown";
}

bool parse_audio_scope_trigger_mode(std::string_view text,
                                    AudioScopeTriggerMode& out) {
    if (text == "none" || text == "off" || text == "raw") {
        out = AudioScopeTriggerMode::kNone;
        return true;
    }
    if (text == "rising_zero" || text == "rising-zero" ||
        text == "risingZero") {
        out = AudioScopeTriggerMode::kRisingZero;
        return true;
    }
    return false;
}

AudioScopeAcquisition acquire_audio_scope_window(
    BufferView<const float> source,
    const AudioScopeAcquisitionConfig& config,
    const AudioProbeSnapshot* snapshot_metadata) {
    AudioScopeAcquisition out;
    out.sample_rate = snapshot_metadata ? snapshot_metadata->sample_rate : 0.0;
    out.source_sequence_number =
        snapshot_metadata ? snapshot_metadata->sequence_number : 0;
    out.source_channel_count = clamp_u32(source.num_channels());
    out.source_frames = clamp_u32(source.num_samples());
    out.selected_channel = config.selected_channel;

    if (config.window_samples == 0) {
        add_warning(out.warnings, "window_samples_must_be_positive");
        return out;
    }
    if (source.empty() || source.num_channels() == 0 || source.num_samples() == 0) {
        add_warning(out.warnings, "empty_source");
        return out;
    }

    std::size_t channel = config.selected_channel;
    if (channel >= source.num_channels()) {
        channel = 0;
        out.selected_channel = 0;
        add_warning(out.warnings, "selected_channel_out_of_range");
    }

    const auto frames = source.num_samples();
    std::size_t window = config.window_samples;
    if (window > frames) {
        window = frames;
        add_warning(out.warnings, "window_truncated_to_source");
    }

    const auto latest_start = frames - window;
    auto start = latest_start;
    bool trigger_found = false;
    std::size_t trigger_sample = 0;
    const auto samples = source.channel(channel);

    if (config.trigger_mode == AudioScopeTriggerMode::kRisingZero &&
        frames >= 2 && window > 0) {
        for (std::size_t i = latest_start; i > 0; --i) {
            if (is_rising_zero(samples[i - 1], samples[i])) {
                start = i;
                trigger_sample = i;
                trigger_found = true;
                break;
            }
        }
        if (!trigger_found) {
            add_warning(out.warnings, "trigger_not_found");
        }
    }

    out.ok = true;
    out.window_start = clamp_u32(start);
    out.window_samples = clamp_u32(window);
    out.trigger_found = trigger_found;
    out.trigger_sample = clamp_u32(trigger_sample);
    out.samples.assign(samples.begin() + static_cast<std::ptrdiff_t>(start),
                       samples.begin() + static_cast<std::ptrdiff_t>(start + window));
    return out;
}

AudioScopeMeasurements measure_audio_scope_window(
    const AudioScopeAcquisition& acquisition) {
    AudioScopeMeasurements out;
    if (!acquisition.ok || acquisition.samples.empty()) {
        out.warnings.push_back("empty_source");
        return out;
    }

    double sum = 0.0;
    double sum_sq = 0.0;
    float min_v = std::numeric_limits<float>::infinity();
    float max_v = -std::numeric_limits<float>::infinity();
    int finite_count = 0;
    int nonfinite_count = 0;

    for (float sample : acquisition.samples) {
        if (!std::isfinite(sample)) {
            ++nonfinite_count;
            continue;
        }
        min_v = std::min(min_v, sample);
        max_v = std::max(max_v, sample);
        sum += static_cast<double>(sample);
        sum_sq += static_cast<double>(sample) * static_cast<double>(sample);
        ++finite_count;
    }

    if (nonfinite_count > 0) {
        out.warnings.push_back("nonfinite_samples_ignored:"
                               + std::to_string(nonfinite_count));
    }
    if (finite_count == 0) {
        out.warnings.push_back("no_finite_samples");
        return out;
    }

    out.peak_to_peak_available = true;
    out.peak_to_peak = static_cast<double>(max_v) - static_cast<double>(min_v);
    out.rms_available = true;
    out.rms = std::sqrt(sum_sq / static_cast<double>(finite_count));
    out.dc_offset_available = true;
    out.dc_offset = sum / static_cast<double>(finite_count);

    constexpr double kSilenceEpsilon = 1.0e-12;
    if (out.rms > kSilenceEpsilon) {
        const auto abs_peak = std::max(std::abs(static_cast<double>(min_v)),
                                       std::abs(static_cast<double>(max_v)));
        out.crest_factor_available = true;
        out.crest_factor = abs_peak / out.rms;
    }

    if (out.peak_to_peak <= kSilenceEpsilon) {
        out.warnings.push_back("frequency_unavailable_silence");
        return out;
    }

    std::vector<double> intervals;
    std::size_t previous_crossing = 0;
    bool have_previous = false;
    for (std::size_t i = 1; i < acquisition.samples.size(); ++i) {
        const float prev = acquisition.samples[i - 1];
        const float current = acquisition.samples[i];
        if (!std::isfinite(prev) || !std::isfinite(current)) continue;
        if (!is_rising_zero(prev, current)) continue;

        if (have_previous) {
            intervals.push_back(static_cast<double>(i - previous_crossing));
        }
        previous_crossing = i;
        have_previous = true;
    }

    if (intervals.size() < 2 || acquisition.sample_rate <= 0.0) {
        out.warnings.push_back("frequency_unavailable_nonperiodic");
        return out;
    }

    const auto minmax = std::minmax_element(intervals.begin(), intervals.end());
    const double average =
        std::accumulate(intervals.begin(), intervals.end(), 0.0) /
        static_cast<double>(intervals.size());
    const double spread = *minmax.second - *minmax.first;
    if (average <= 0.0 ||
        (spread / average) > kAudioScopeFrequencyPeriodTolerance) {
        out.warnings.push_back("frequency_unavailable_nonperiodic");
        return out;
    }

    out.frequency_available = true;
    out.period_samples = average;
    out.frequency_hz = acquisition.sample_rate / average;
    return out;
}

}  // namespace pulp::audio
