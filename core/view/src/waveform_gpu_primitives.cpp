#include <pulp/view/waveform_gpu_primitives.hpp>

#include <algorithm>
#include <cmath>
#include <limits>
#include <utility>

namespace pulp::view {
namespace {

std::uint32_t positive_ceil_px(float value) noexcept {
    if (!(value > 0.0f) || !std::isfinite(value)) return 0;
    const auto rounded = std::ceil(static_cast<double>(value));
    if (rounded >= static_cast<double>(std::numeric_limits<std::uint32_t>::max())) {
        return std::numeric_limits<std::uint32_t>::max();
    }
    return static_cast<std::uint32_t>(rounded);
}

std::int32_t quantize_milli_px(float value) noexcept {
    if (!std::isfinite(value)) return 0;
    const auto scaled = std::llround(static_cast<double>(value) * 1000.0);
    if (scaled > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    if (scaled < std::numeric_limits<std::int32_t>::min()) {
        return std::numeric_limits<std::int32_t>::min();
    }
    return static_cast<std::int32_t>(scaled);
}

WaveformViewport normalized_viewport_for_thumbnail(const pulp::audio::WaveformOverview& thumbnail,
                                                   const WaveformViewport& viewport) {
    auto normalized = viewport;
    const auto info = thumbnail.info();
    const auto frames = info.num_source_frames >
                                static_cast<std::uint64_t>(std::numeric_limits<int64_t>::max())
                            ? std::numeric_limits<int64_t>::max()
                            : static_cast<int64_t>(info.num_source_frames);
    const auto requested_length = viewport.visible_length > 0 ? viewport.visible_length : frames;
    normalized.set_total_samples(frames);
    normalized.set_visible_range(viewport.visible_start, requested_length);
    return normalized;
}

std::uint32_t clamped_u32(std::uint64_t value) noexcept {
    return value > std::numeric_limits<std::uint32_t>::max()
               ? std::numeric_limits<std::uint32_t>::max()
               : static_cast<std::uint32_t>(value);
}

bool peak_sample_to_int64(std::uint32_t peak_index,
                          std::uint32_t samples_per_peak,
                          int64_t& sample_out) noexcept {
    const auto sample = static_cast<std::uint64_t>(peak_index) *
                        static_cast<std::uint64_t>(samples_per_peak);
    if (sample > static_cast<std::uint64_t>(std::numeric_limits<int64_t>::max())) {
        return false;
    }
    sample_out = static_cast<int64_t>(sample);
    return true;
}

bool read_peak(const pulp::audio::ThumbnailLevel& level,
               std::uint32_t channel,
               std::uint32_t peak_index,
               float& min_value,
               float& max_value) noexcept {
    if (peak_index >= level.peaks_per_channel) return false;

    if (channel == pulp::audio::WaveformOverview::kAllChannels) {
        if (level.peaks.empty()) return false;
        min_value = 1.0f;
        max_value = -1.0f;
        bool found = false;
        for (const auto& channel_peaks : level.peaks) {
            if (peak_index >= channel_peaks.size()) continue;
            const auto peak = channel_peaks[peak_index];
            min_value = std::min(min_value, peak.min());
            max_value = std::max(max_value, peak.max());
            found = true;
        }
        return found;
    }

    if (channel >= level.peaks.size()) return false;
    const auto& channel_peaks = level.peaks[channel];
    if (peak_index >= channel_peaks.size()) return false;

    const auto peak = channel_peaks[peak_index];
    min_value = peak.min();
    max_value = peak.max();
    return true;
}

} // namespace

bool WaveformGpuUploadKey::valid() const noexcept {
    return source_generation != 0 && samples_per_peak > 0 && peak_count > 0 &&
           visible_length > 0 && target_width_px > 0 && target_height_px > 0;
}

bool operator==(const WaveformGpuUploadKey& a,
                const WaveformGpuUploadKey& b) noexcept {
    return a.source_generation == b.source_generation &&
           a.channel == b.channel &&
           a.thumbnail_level_index == b.thumbnail_level_index &&
           a.samples_per_peak == b.samples_per_peak &&
           a.first_peak == b.first_peak &&
           a.peak_count == b.peak_count &&
           a.visible_start == b.visible_start &&
           a.visible_length == b.visible_length &&
           a.bounds_x_milli_px == b.bounds_x_milli_px &&
           a.bounds_y_milli_px == b.bounds_y_milli_px &&
           a.bounds_width_milli_px == b.bounds_width_milli_px &&
           a.bounds_height_milli_px == b.bounds_height_milli_px &&
           a.target_width_px == b.target_width_px &&
           a.target_height_px == b.target_height_px;
}

bool operator!=(const WaveformGpuUploadKey& a,
                const WaveformGpuUploadKey& b) noexcept {
    return !(a == b);
}

WaveformGpuStaticLayerPlan build_waveform_gpu_static_layer_plan(
    const pulp::audio::WaveformOverview& thumbnail,
    const WaveformViewport& viewport,
    const WaveformGpuLayerConfig& config) {
    WaveformGpuStaticLayerPlan plan;
    plan.prefer_gpu = config.prefer_gpu;

    if (config.source_generation == 0) return plan;

    const auto info = thumbnail.info();
    if (thumbnail.empty() || info.num_source_frames == 0 ||
        info.num_channels == 0 || thumbnail.num_levels() == 0) {
        return plan;
    }
    if (config.channel != pulp::audio::WaveformOverview::kAllChannels &&
        config.channel >= info.num_channels) {
        return plan;
    }

    auto normalized = normalized_viewport_for_thumbnail(thumbnail, viewport);
    if (normalized.empty()) return plan;

    const auto target_width_px = positive_ceil_px(normalized.bounds.width);
    const auto target_height_px = positive_ceil_px(normalized.bounds.height);
    if (target_width_px == 0 || target_height_px == 0) return plan;

    auto target_columns = target_width_px;
    if (config.max_columns > 0) {
        target_columns = std::min(target_columns, config.max_columns);
    }
    if (target_columns == 0) return plan;

    const auto level_index = thumbnail.best_level_for(target_columns);
    if (level_index >= thumbnail.num_levels()) return plan;
    const auto& level = thumbnail.level(level_index);
    if (level.samples_per_peak == 0 || level.peaks_per_channel == 0) return plan;

    const auto visible_start = static_cast<std::uint64_t>(std::max<int64_t>(0, normalized.visible_start));
    const auto visible_end = static_cast<std::uint64_t>(std::max<int64_t>(0, normalized.visible_end()));
    const auto samples_per_peak = static_cast<std::uint64_t>(level.samples_per_peak);
    const auto first_peak = visible_start / samples_per_peak;
    const auto end_peak = (visible_end + samples_per_peak - 1u) / samples_per_peak;
    const auto max_peaks = static_cast<std::uint64_t>(level.peaks_per_channel);
    if (first_peak >= max_peaks || end_peak <= first_peak) return plan;

    const auto clamped_end_peak = std::min(end_peak, max_peaks);
    const auto peak_count = clamped_end_peak - first_peak;
    if (peak_count == 0) return plan;

    plan.valid = true;
    plan.viewport = normalized;
    plan.upload_key.source_generation = config.source_generation;
    plan.upload_key.channel = config.channel;
    plan.upload_key.thumbnail_level_index = clamped_u32(level_index);
    plan.upload_key.samples_per_peak = level.samples_per_peak;
    plan.upload_key.first_peak = clamped_u32(first_peak);
    plan.upload_key.peak_count = clamped_u32(peak_count);
    plan.upload_key.visible_start = normalized.visible_start;
    plan.upload_key.visible_length = normalized.visible_length;
    plan.upload_key.bounds_x_milli_px = quantize_milli_px(normalized.bounds.x);
    plan.upload_key.bounds_y_milli_px = quantize_milli_px(normalized.bounds.y);
    plan.upload_key.bounds_width_milli_px = quantize_milli_px(normalized.bounds.width);
    plan.upload_key.bounds_height_milli_px = quantize_milli_px(normalized.bounds.height);
    plan.upload_key.target_width_px = target_width_px;
    plan.upload_key.target_height_px = target_height_px;
    plan.vertex_count = plan.upload_key.peak_count;
    plan.upload_bytes = plan.vertex_count * sizeof(WaveformPeakVertex);
    plan.cpu_fallback_available = true;
    return plan;
}

WaveformGpuLayerPlan build_waveform_gpu_layer_plan(
    const pulp::audio::WaveformOverview& thumbnail,
    const WaveformViewport& viewport,
    int64_t playhead_sample,
    const WaveformGpuLayerConfig& config,
    const WaveformGpuUploadKey* previous_static_key) {
    WaveformGpuLayerPlan plan;
    plan.static_layer = build_waveform_gpu_static_layer_plan(thumbnail, viewport, config);
    if (!plan.static_layer.valid) return plan;

    plan.playhead = build_waveform_playhead_overlay(plan.static_layer.viewport, playhead_sample);
    plan.static_layer_dirty = previous_static_key == nullptr ||
                              *previous_static_key != plan.static_layer.upload_key;
    plan.playhead_only_redraw = !plan.static_layer_dirty && plan.playhead.visible;
    return plan;
}

std::size_t fill_waveform_peak_vertices(
    const pulp::audio::WaveformOverview& thumbnail,
    const WaveformGpuStaticLayerPlan& plan,
    std::span<WaveformPeakVertex> dst) noexcept {
    if (!plan.valid || !plan.upload_key.valid() || dst.empty()) return 0;
    if (plan.upload_key.thumbnail_level_index >= thumbnail.num_levels()) return 0;

    const auto& level = thumbnail.level(plan.upload_key.thumbnail_level_index);
    if (level.samples_per_peak != plan.upload_key.samples_per_peak ||
        level.peaks_per_channel == 0) {
        return 0;
    }

    const auto count = std::min<std::size_t>(dst.size(), plan.upload_key.peak_count);
    const auto center_y = plan.viewport.bounds.y + plan.viewport.bounds.height * 0.5f;
    const auto half_h = std::max(0.0f, plan.viewport.bounds.height * 0.5f);

    std::size_t written = 0;
    for (; written < count; ++written) {
        const auto peak_index = plan.upload_key.first_peak + static_cast<std::uint32_t>(written);
        float min_value = 0.0f;
        float max_value = 0.0f;
        if (!read_peak(level, plan.upload_key.channel, peak_index, min_value, max_value)) break;
        if (min_value > max_value) std::swap(min_value, max_value);

        int64_t sample = 0;
        if (!peak_sample_to_int64(peak_index, plan.upload_key.samples_per_peak, sample)) break;
        const auto y_top = center_y - max_value * half_h;
        const auto y_bottom = center_y - min_value * half_h;

        auto& vertex = dst[written];
        vertex.x = plan.viewport.sample_to_x(sample);
        vertex.y_min = std::min(y_top, y_bottom);
        vertex.y_max = std::max(y_top, y_bottom);
        vertex.min_value = min_value;
        vertex.max_value = max_value;
        vertex.source_peak_index = peak_index;
    }
    return written;
}

bool WaveformGpuResourceRecord::valid() const noexcept {
    return resource_id != 0 && backend_generation != 0 && key.valid();
}

WaveformGpuResourceCache::WaveformGpuResourceCache(std::size_t capacity) {
    prepare(capacity);
}

bool WaveformGpuResourceCache::prepare(
    std::size_t capacity,
    std::vector<WaveformGpuResourceRecord>* evicted_records) {
    if (capacity < records_.size() && evicted_records == nullptr) return false;

    if (capacity == 0) {
        if (evicted_records) {
            evicted_records->insert(evicted_records->end(), records_.begin(), records_.end());
        }
        capacity_ = 0;
        evictions_ += records_.size();
        records_.clear();
        return true;
    }
    try {
        records_.reserve(capacity);
    } catch (...) {
        return false;
    }
    capacity_ = capacity;
    while (records_.size() > capacity_) {
        auto victim = std::min_element(records_.begin(), records_.end(), [](const auto& a, const auto& b) {
            return a.last_used < b.last_used;
        });
        if (victim == records_.end()) break;
        if (evicted_records) evicted_records->push_back(*victim);
        records_.erase(victim);
        ++evictions_;
    }
    return true;
}

std::vector<WaveformGpuResourceRecord> WaveformGpuResourceCache::clear_and_return_records() {
    std::vector<WaveformGpuResourceRecord> removed;
    records_.swap(removed);
    return removed;
}

WaveformGpuResourcePutResult WaveformGpuResourceCache::put(
    const WaveformGpuUploadKey& key,
    std::uint64_t resource_id,
    std::size_t bytes,
    std::uint64_t backend_generation) {
    WaveformGpuResourcePutResult result;
    if (!key.valid() || resource_id == 0 || bytes == 0 || backend_generation == 0 ||
        capacity_ == 0) {
        return result;
    }

    const auto stamp = ++clock_;
    for (auto& record : records_) {
        if (record.key == key) {
            if (backend_generation < record.backend_generation) return result;
            result.ok = true;
            result.replaced = true;
            result.replaced_record = record;
            record.resource_id = resource_id;
            record.bytes = bytes;
            record.backend_generation = backend_generation;
            record.last_used = stamp;
            return result;
        }
    }

    if (records_.size() < capacity_) {
        records_.push_back({key, resource_id, bytes, backend_generation, stamp});
        result.ok = true;
        return result;
    }

    auto victim = std::min_element(records_.begin(), records_.end(), [](const auto& a, const auto& b) {
        return a.last_used < b.last_used;
    });
    if (victim == records_.end()) return result;
    result.ok = true;
    result.evicted = true;
    result.evicted_record = *victim;
    *victim = {key, resource_id, bytes, backend_generation, stamp};
    ++evictions_;
    return result;
}

const WaveformGpuResourceRecord* WaveformGpuResourceCache::find(
    const WaveformGpuUploadKey& key) noexcept {
    for (auto& record : records_) {
        if (record.key == key) {
            record.last_used = ++clock_;
            ++hits_;
            return &record;
        }
    }
    ++misses_;
    return nullptr;
}

const WaveformGpuResourceRecord* WaveformGpuResourceCache::find(
    const WaveformGpuUploadKey& key,
    std::uint64_t backend_generation) noexcept {
    if (backend_generation == 0) {
        ++misses_;
        return nullptr;
    }
    for (auto& record : records_) {
        if (record.key == key && record.backend_generation == backend_generation) {
            record.last_used = ++clock_;
            ++hits_;
            return &record;
        }
    }
    ++misses_;
    return nullptr;
}

bool WaveformGpuResourceCache::erase(
    const WaveformGpuUploadKey& key,
    WaveformGpuResourceRecord& removed_record) noexcept {
    const auto it = std::find_if(records_.begin(), records_.end(), [&](const auto& record) {
        return record.key == key;
    });
    if (it == records_.end()) return false;
    removed_record = *it;
    records_.erase(it);
    return true;
}

WaveformGpuResourceCacheStats WaveformGpuResourceCache::stats() const noexcept {
    WaveformGpuResourceCacheStats stats;
    stats.entries = records_.size();
    stats.capacity = capacity_;
    stats.hits = hits_;
    stats.misses = misses_;
    stats.evictions = evictions_;
    for (const auto& record : records_) {
        stats.bytes += record.bytes;
    }
    return stats;
}

} // namespace pulp::view
