#include <pulp/audio/waveform_overview.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>
#include <vector>

namespace pulp::audio {

namespace {

ThumbnailLevel decimate_level(const ThumbnailLevel& src) {
    ThumbnailLevel out;
    out.samples_per_peak = src.samples_per_peak * 2u;
    const std::size_t in_peaks = src.peaks_per_channel;
    const std::size_t out_peaks = (in_peaks + 1u) / 2u;
    out.peaks_per_channel = static_cast<uint32_t>(out_peaks);
    out.peaks.resize(src.peaks.size());

    for (std::size_t ch = 0; ch < src.peaks.size(); ++ch) {
        const auto& in = src.peaks[ch];
        auto& dst = out.peaks[ch];
        dst.resize(out_peaks);
        for (std::size_t i = 0; i < out_peaks; ++i) {
            const std::size_t a = i * 2;
            const std::size_t b = std::min(a + 1, in_peaks - 1);
            const AudioPeak& pa = in[a];
            const AudioPeak& pb = in[b];
            AudioPeak merged;
            merged.min_q7 = std::min(pa.min_q7, pb.min_q7);
            merged.max_q7 = std::max(pa.max_q7, pb.max_q7);
            dst[i] = merged;
        }
    }
    return out;
}

}  // namespace

WaveformOverview WaveformOverview::build_from_buffer_view(
    BufferView<const float> source,
    uint32_t sample_rate,
    uint32_t samples_per_peak) {
    WaveformOverview t;
    if (source.num_channels() == 0 || source.num_samples() == 0 || samples_per_peak == 0) {
        return t;
    }

    t.num_channels_ = static_cast<uint32_t>(source.num_channels());
    t.num_source_frames_ = static_cast<uint64_t>(source.num_samples());
    t.sample_rate_ = sample_rate;

    // Base level.
    ThumbnailLevel base;
    base.samples_per_peak = samples_per_peak;
    const uint64_t frames = static_cast<uint64_t>(source.num_samples());
    const uint32_t peaks =
        static_cast<uint32_t>((frames + samples_per_peak - 1u) / samples_per_peak);
    base.peaks_per_channel = peaks;
    base.peaks.resize(source.num_channels());

    for (std::size_t ch = 0; ch < source.num_channels(); ++ch) {
        const auto* src = source.channel_ptr(ch);
        auto& dst = base.peaks[ch];
        dst.resize(peaks);
        for (uint32_t p = 0; p < peaks; ++p) {
            const std::size_t a = static_cast<std::size_t>(p) * samples_per_peak;
            const std::size_t b = std::min<std::size_t>(a + samples_per_peak,
                                                        source.num_samples());
            float lo =  std::numeric_limits<float>::infinity();
            float hi = -std::numeric_limits<float>::infinity();
            for (std::size_t i = a; i < b; ++i) {
                const float s = src[i];
                if (s < lo) lo = s;
                if (s > hi) hi = s;
            }
            if (!std::isfinite(lo) || !std::isfinite(hi)) {
                lo = 0.0f;
                hi = 0.0f;
            }
            dst[p] = AudioPeak::from_range(lo, hi);
        }
    }
    t.levels_.push_back(std::move(base));

    // Decimate until level holds <= 16 peaks per channel.
    while (t.levels_.back().peaks_per_channel > 16u) {
        t.levels_.push_back(decimate_level(t.levels_.back()));
    }

    return t;
}

WaveformOverview WaveformOverview::build_from_buffer(
    const AudioFileData& data,
    uint32_t samples_per_peak) {
    if (data.empty() || samples_per_peak == 0) {
        return {};
    }

    auto frames = data.channels[0].size();
    for (const auto& channel : data.channels) {
        frames = std::min(frames, channel.size());
    }
    if (frames == 0) return {};

    std::vector<const float*> ptrs(data.channels.size(), nullptr);
    for (std::size_t ch = 0; ch < data.channels.size(); ++ch) {
        ptrs[ch] = data.channels[ch].data();
    }
    return build_from_buffer_view(
        BufferView<const float>(ptrs.data(), data.num_channels(), frames),
        data.sample_rate,
        samples_per_peak);
}

std::optional<WaveformOverview> WaveformOverview::build_from_path(
    const std::string& path,
    uint32_t samples_per_peak) {
    auto data = read_audio_file(path);
    if (!data) return std::nullopt;
    if (data->empty()) return std::nullopt;
    return build_from_buffer(*data, samples_per_peak);
}

WaveformOverviewInfo WaveformOverview::info() const noexcept {
    WaveformOverviewInfo i;
    i.num_channels = num_channels_;
    i.num_source_frames = num_source_frames_;
    i.sample_rate = sample_rate_;
    i.num_levels = levels_.size();
    std::size_t bytes = 0;
    for (const auto& lvl : levels_) {
        for (const auto& ch : lvl.peaks) {
            bytes += ch.size() * sizeof(AudioPeak);
        }
    }
    i.bytes_used = bytes;
    return i;
}

std::size_t WaveformOverview::best_level_for(uint32_t target_peaks) const noexcept {
    if (levels_.empty()) return 0;
    if (target_peaks == 0) return levels_.size() - 1;
    // Walk from coarsest to finest, return the first level that has at
    // least target_peaks-per-channel.
    for (std::size_t i = levels_.size(); i > 0; --i) {
        const std::size_t idx = i - 1;
        if (levels_[idx].peaks_per_channel >= target_peaks) return idx;
    }
    return 0;  // finest available
}

std::size_t WaveformOverview::render_min_max(uint32_t channel,
                                           uint32_t target_peaks,
                                           float* dst) const noexcept {
    if (dst == nullptr || target_peaks == 0 || levels_.empty()) {
        return 0;
    }

    const std::size_t lvl_idx = best_level_for(target_peaks);
    const ThumbnailLevel& lvl = levels_[lvl_idx];
    if (lvl.peaks_per_channel == 0) return 0;

    const std::size_t channels = lvl.peaks.size();
    const bool fold_all = (channel == kAllChannels) || (channel >= channels);

    const std::size_t src_count = lvl.peaks_per_channel;
    const std::size_t want = target_peaks;
    for (std::size_t p = 0; p < want; ++p) {
        // Map output index p in [0, want) to a span over the source
        // [a, b) inside [0, src_count) so we never sample past the end.
        const std::size_t a = (p * src_count) / want;
        std::size_t b = ((p + 1) * src_count) / want;
        if (b <= a) b = a + 1;
        if (b > src_count) b = src_count;

        float lo =  std::numeric_limits<float>::infinity();
        float hi = -std::numeric_limits<float>::infinity();

        if (fold_all) {
            for (std::size_t i = a; i < b; ++i) {
                for (std::size_t ch = 0; ch < channels; ++ch) {
                    const AudioPeak& pk = lvl.peaks[ch][i];
                    const float l = pk.min();
                    const float h = pk.max();
                    if (l < lo) lo = l;
                    if (h > hi) hi = h;
                }
            }
        } else {
            const auto& src = lvl.peaks[channel];
            for (std::size_t i = a; i < b; ++i) {
                const AudioPeak& pk = src[i];
                const float l = pk.min();
                const float h = pk.max();
                if (l < lo) lo = l;
                if (h > hi) hi = h;
            }
        }

        if (!std::isfinite(lo)) lo = 0.0f;
        if (!std::isfinite(hi)) hi = 0.0f;
        dst[p * 2 + 0] = lo;
        dst[p * 2 + 1] = hi;
    }
    return want;
}

}  // namespace pulp::audio
