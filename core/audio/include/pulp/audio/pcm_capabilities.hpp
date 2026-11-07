#pragma once

/// @file pcm_capabilities.hpp
/// Turns what a PCM driver reports (rate range, channel range, period range)
/// into the lists a `DeviceInfo` carries. The conversion is plain arithmetic
/// on numbers, so it lives apart from any platform audio API and can be tested
/// on every host.

#include <pulp/audio/device.hpp>

#include <algorithm>
#include <array>
#include <cstdint>
#include <iterator>
#include <optional>
#include <vector>

namespace pulp::audio {

/// What one direction (playback or capture) of a device can do.
struct PcmCapabilities {
    unsigned rate_min = 0;
    unsigned rate_max = 0;
    /// Rates the driver confirmed one by one. Only meaningful when
    /// `rates_tested` is true.
    std::vector<unsigned> accepted_rates;
    bool rates_tested = false;

    unsigned channels_min = 0;
    unsigned channels_max = 0;

    /// Frames per callback the driver will accept.
    unsigned long period_min = 0;
    unsigned long period_max = 0;
    /// Largest total buffer in frames; 0 when the driver gives no bound.
    unsigned long buffer_max = 0;
};

/// Rates worth offering when a driver reports a continuous range.
inline const std::array<unsigned, 13>& standard_sample_rates() {
    static const std::array<unsigned, 13> rates{8000,  11025, 16000,  22050,  32000,  44100, 48000,
                                                88200, 96000, 176400, 192000, 352800, 384000};
    return rates;
}

/// Sample rates the device supports, ascending. Rates the driver confirmed are
/// used as they are; without confirmation the standard rates inside the
/// reported range stand in. A device that supports exactly one rate reports it
/// even when it is not a standard one.
inline std::vector<double> sample_rates_from(const PcmCapabilities& caps) {
    std::vector<double> out;
    if (caps.rate_min == 0 || caps.rate_max < caps.rate_min)
        return out;
    auto in_range = [&](unsigned r) { return r >= caps.rate_min && r <= caps.rate_max; };

    std::vector<unsigned> picked;
    if (caps.rates_tested) {
        for (unsigned r : caps.accepted_rates) {
            if (in_range(r))
                picked.push_back(r);
        }
    } else {
        for (unsigned r : standard_sample_rates()) {
            if (in_range(r))
                picked.push_back(r);
        }
    }
    if (picked.empty() && caps.rate_min == caps.rate_max)
        picked.push_back(caps.rate_min);

    std::sort(picked.begin(), picked.end());
    picked.erase(std::unique(picked.begin(), picked.end()), picked.end());
    out.reserve(picked.size());
    for (unsigned r : picked)
        out.push_back(static_cast<double>(r));
    return out;
}

/// Callback buffer sizes (frames), ascending: the powers of two inside the
/// driver's period range such that `periods_per_buffer` periods still fit in
/// the largest buffer. A driver with one fixed period reports that period.
inline std::vector<int> buffer_sizes_from(const PcmCapabilities& caps,
                                          unsigned periods_per_buffer = 2) {
    std::vector<int> out;
    if (caps.period_min == 0 || caps.period_max < caps.period_min)
        return out;
    if (periods_per_buffer == 0)
        periods_per_buffer = 1;
    auto fits = [&](unsigned long p) {
        if (p < caps.period_min || p > caps.period_max)
            return false;
        if (p > 0x7FFFFFFFul)
            return false;
        return caps.buffer_max == 0 || p * periods_per_buffer <= caps.buffer_max;
    };
    for (unsigned long p = 16; p <= 16384; p *= 2) {
        if (fits(p))
            out.push_back(static_cast<int>(p));
    }
    if (out.empty() && fits(caps.period_min))
        out.push_back(static_cast<int>(caps.period_min));
    return out;
}

namespace detail {
template <typename T>
std::vector<T> intersect_sorted(const std::vector<T>& a, const std::vector<T>& b) {
    std::vector<T> out;
    std::set_intersection(a.begin(), a.end(), b.begin(), b.end(), std::back_inserter(out));
    return out;
}
} // namespace detail

/// Fill the capability fields of `info` from the probed directions. A device
/// opened for both directions must run one rate and one buffer size on both,
/// so the lists are the intersection of the directions that probed. Channel
/// counts are per direction. When neither direction could be probed (device
/// busy, or no audio hardware) a conservative default list is reported so
/// callers never see an empty one.
inline void fill_device_info_from_capabilities(DeviceInfo& info,
                                               const std::optional<PcmCapabilities>& playback,
                                               const std::optional<PcmCapabilities>& capture) {
    auto usable = [](const std::optional<PcmCapabilities>& c) {
        return c && c->channels_max > 0 && c->channels_min <= c->channels_max;
    };
    const bool has_play = usable(playback);
    const bool has_cap = usable(capture);

    if (!has_play && !has_cap) {
        info.max_output_channels = 2;
        info.max_input_channels = 2;
        info.sample_rates = {44100, 48000, 88200, 96000};
        info.buffer_sizes = {64, 128, 256, 512, 1024, 2048, 4096};
        return;
    }

    info.max_output_channels = has_play ? static_cast<int>(playback->channels_max) : 0;
    info.max_input_channels = has_cap ? static_cast<int>(capture->channels_max) : 0;

    if (has_play && has_cap) {
        info.sample_rates =
            detail::intersect_sorted(sample_rates_from(*playback), sample_rates_from(*capture));
        info.buffer_sizes =
            detail::intersect_sorted(buffer_sizes_from(*playback), buffer_sizes_from(*capture));
    } else {
        const auto& only = has_play ? *playback : *capture;
        info.sample_rates = sample_rates_from(only);
        info.buffer_sizes = buffer_sizes_from(only);
    }
}

} // namespace pulp::audio
