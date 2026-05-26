#pragma once

// AudioThumbnail + AudioThumbnailCache
// ─────────────────────────────────────────────────────────────────────────────
// Pre-decoded min/max peak summaries over an audio file at multiple zoom
// levels. The peaks live in memory (and optionally on disk) so that a
// waveform widget can redraw at any zoom without re-touching the source
// audio.
//
// Design:
//   * One AudioThumbnail per source. Peaks are stored per channel as
//     interleaved (min, max) int8 samples normalised to [-1, 1]. A single
//     int8-pair costs 2 bytes per channel per peak — tens of thousands of
//     peaks fit in a few KB.
//   * Higher zoom levels are decimations of the level below. We build the
//     base "samples_per_peak" level by walking the audio source once, then
//     decimate by 2x for each subsequent level until we are down to a
//     handful of peaks.
//   * AudioThumbnailCache is a tiny LRU keyed by source path. Eviction is
//     by configurable maximum entry count.
//   * Optional on-disk persistence — call `set_disk_cache_dir(path)` to
//     point the cache at a directory. After that, `get_or_build()` first
//     consults the disk cache (keyed by SHA-256 of "<source-path>|<mtime>")
//     and writes a `.thumb` file on every cache insert. Re-opening Pulp
//     finds cached thumbnails instantly without re-decoding the source.
//     Disk cache files are versioned with a magic header so old / corrupt
//     entries are silently ignored.
//
// Wired into core/view::WaveformView so that callers may pass either a raw
// sample buffer (existing behaviour) or an AudioThumbnail* (new behaviour
// — peaks render straight from the cached table).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <list>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <pulp/audio/audio_file.hpp>

namespace pulp::audio {

// A single (min, max) pair at one peak slot for one channel.
// Stored normalised to [-1, 1]. int8 is plenty for waveform display
// (256 levels per axis) and keeps peak tables small.
struct AudioPeak {
    int8_t min_q7 = 0;
    int8_t max_q7 = 0;

    float min() const noexcept { return static_cast<float>(min_q7) / 127.0f; }
    float max() const noexcept { return static_cast<float>(max_q7) / 127.0f; }

    static AudioPeak from_range(float lo, float hi) noexcept {
        AudioPeak p;
        if (lo < -1.0f) lo = -1.0f;
        if (lo >  1.0f) lo =  1.0f;
        if (hi < -1.0f) hi = -1.0f;
        if (hi >  1.0f) hi =  1.0f;
        p.min_q7 = static_cast<int8_t>(lo * 127.0f);
        p.max_q7 = static_cast<int8_t>(hi * 127.0f);
        return p;
    }
};

// One zoom level: a peak table for every channel.
// peaks[channel] has `peaks_per_channel` entries.
struct ThumbnailLevel {
    uint32_t samples_per_peak = 0;
    uint32_t peaks_per_channel = 0;
    std::vector<std::vector<AudioPeak>> peaks;  // [channel][peak_index]
};

// Lightweight summary of an in-memory thumbnail.
struct AudioThumbnailInfo {
    uint32_t num_channels = 0;
    uint64_t num_source_frames = 0;
    uint32_t sample_rate = 0;
    std::size_t num_levels = 0;
    std::size_t bytes_used = 0;
};

class AudioThumbnail {
public:
    // Build from an audio-file path. Returns nullopt if the source file
    // cannot be opened or has zero frames.
    //
    // `samples_per_peak` is the resolution of the base level (level 0).
    // Subsequent levels are 2x decimations. 256 is a sensible default for
    // typical UI scrubbing.
    //
    // This call is synchronous — the caller decides whether to run it on a
    // background thread.
    static std::optional<AudioThumbnail> build_from_path(
        const std::string& path,
        uint32_t samples_per_peak = 256);

    // Build from an in-memory decoded buffer. Useful for tests and for
    // sources that are already loaded.
    static AudioThumbnail build_from_buffer(
        const AudioFileData& data,
        uint32_t samples_per_peak = 256);

    AudioThumbnailInfo info() const noexcept;

    const ThumbnailLevel& level(std::size_t i) const { return levels_.at(i); }
    std::size_t num_levels() const noexcept { return levels_.size(); }

    // Pick the level whose peaks-per-channel is closest to (and at least
    // as large as) `target_peaks`. If no level is fine enough, returns the
    // finest available (level 0). If the thumbnail is empty, returns
    // index 0; callers must check num_levels().
    std::size_t best_level_for(uint32_t target_peaks) const noexcept;

    // Produce a flattened, decimated (min, max) pair stream at any target
    // resolution. Caller passes a destination of size `target_peaks * 2`
    // where each pair is (min, max) in [-1, 1]. `channel == UINT32_MAX`
    // means "fold all channels" (max-of-max / min-of-min). Returns the
    // number of pairs actually written.
    static constexpr uint32_t kAllChannels = static_cast<uint32_t>(-1);
    std::size_t render_min_max(uint32_t channel,
                                uint32_t target_peaks,
                                float* dst_min_max_interleaved) const noexcept;

    bool empty() const noexcept { return levels_.empty(); }

    // Internal: rebuild a thumbnail from a serialized blob. Public so the
    // free-function `deserialize_thumbnail()` (defined in the .cpp) can
    // construct one without becoming a friend. Returns nullopt on malformed
    // input. Not part of the supported API surface — call
    // `deserialize_thumbnail()` instead.
    static std::optional<AudioThumbnail> from_serialized_levels(
        uint32_t num_channels,
        uint64_t num_source_frames,
        uint32_t sample_rate,
        uint32_t num_levels,
        const uint8_t* data,
        std::size_t size,
        std::size_t offset);

private:
    std::vector<ThumbnailLevel> levels_;
    uint32_t num_channels_ = 0;
    uint64_t num_source_frames_ = 0;
    uint32_t sample_rate_ = 0;
};

// Serialize/deserialize a thumbnail to a versioned blob. The on-disk
// format is intentionally simple — `magic(4) version(2) num_channels(4)
// num_source_frames(8) sample_rate(4) num_levels(4) [per-level:
// samples_per_peak(4) peaks_per_channel(4) channels * peaks * AudioPeak]`
// — little-endian. The version field is bumped only when the layout
// changes; `deserialize_thumbnail()` rejects mismatched versions and
// returns nullopt.
std::vector<uint8_t> serialize_thumbnail(const AudioThumbnail& t);
std::optional<AudioThumbnail> deserialize_thumbnail(const uint8_t* data,
                                                    std::size_t size);

// ─────────────────────────────────────────────────────────────────────────
// AudioThumbnailCache — in-memory LRU of thumbnails keyed by source path,
// with optional on-disk persistence layer.
//
// Thread-safe. Used by widgets that may rebuild the same waveform many
// times (e.g. WaveformView opened repeatedly with the same sample).
// ─────────────────────────────────────────────────────────────────────────

struct AudioThumbnailCacheStats {
    std::size_t hits = 0;
    std::size_t misses = 0;
    std::size_t evictions = 0;
    std::size_t entries = 0;
    std::size_t capacity = 0;
    std::size_t disk_hits = 0;    ///< get_or_build → loaded from disk
    std::size_t disk_writes = 0;  ///< put → wrote to disk
};

class AudioThumbnailCache {
public:
    explicit AudioThumbnailCache(std::size_t max_entries = 32);

    // Returns a shared_ptr to the thumbnail for `path`, building it on
    // demand. Returns nullptr if the source cannot be opened.
    std::shared_ptr<const AudioThumbnail> get_or_build(
        const std::string& path,
        uint32_t samples_per_peak = 256);

    // Returns the cached thumbnail if present, otherwise nullptr. Does
    // NOT build. Records a hit or miss in stats.
    std::shared_ptr<const AudioThumbnail> get(const std::string& path);

    // Insert a pre-built thumbnail. Used for tests and for sources that
    // were built from a buffer rather than a path.
    void put(const std::string& path,
             std::shared_ptr<const AudioThumbnail> thumbnail);

    // Drop everything.
    void clear();

    // Drop a single entry. Returns true if it was present.
    bool erase(const std::string& path);

    // Atomic snapshot of cache stats. Cheap.
    AudioThumbnailCacheStats stats() const;

    std::size_t size() const;
    std::size_t capacity() const noexcept { return capacity_; }

    // Resize the cache. If the new capacity is smaller, evicts LRU
    // entries to fit.
    void set_capacity(std::size_t max_entries);

    // Point the cache at a directory for on-disk thumbnail persistence.
    // Pass an empty path to disable disk caching. The directory is
    // created on demand. Idempotent — safe to call repeatedly.
    void set_disk_cache_dir(const std::string& dir);
    const std::string& disk_cache_dir() const noexcept { return disk_dir_; }

    // Best-effort: drop every `.thumb` file under the disk cache dir.
    // Does nothing if no dir is configured.
    void clear_disk_cache();

private:
    struct Entry {
        std::string key;
        std::shared_ptr<const AudioThumbnail> value;
    };

    // LRU: front == most recently used.
    using List = std::list<Entry>;
    using Map  = std::unordered_map<std::string, List::iterator>;

    void touch_locked(Map::iterator it);
    void evict_to_capacity_locked();

    // Disk-cache helpers — return nullptr / false on any I/O failure.
    // Both are safe to call with disk_dir_ empty; they no-op silently.
    std::shared_ptr<const AudioThumbnail> load_from_disk(const std::string& path) const;
    bool write_to_disk(const std::string& path,
                       const AudioThumbnail& thumbnail) const;
    std::string disk_path_for(const std::string& source_path) const;

    mutable std::mutex mtx_;
    List list_;
    Map  index_;
    std::size_t capacity_;
    std::string disk_dir_;  // empty == disk caching disabled

    // Stats — relaxed atomics; reads can race with writes harmlessly.
    std::atomic<std::size_t> hits_{0};
    std::atomic<std::size_t> misses_{0};
    std::atomic<std::size_t> evictions_{0};
    std::atomic<std::size_t> disk_hits_{0};
    std::atomic<std::size_t> disk_writes_{0};
};

}  // namespace pulp::audio
