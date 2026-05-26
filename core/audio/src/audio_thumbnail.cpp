#include <pulp/audio/audio_thumbnail.hpp>

#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <limits>
#include <system_error>
#include <utility>

namespace pulp::audio {

// ─────────────────────────────────────────────────────────────────────────
// On-disk thumbnail blob format
// ─────────────────────────────────────────────────────────────────────────
//
// Magic     : 4 bytes  "PTHM"
// Version   : 2 bytes  little-endian (currently 1)
// num_channels      : 4 bytes LE
// num_source_frames : 8 bytes LE
// sample_rate       : 4 bytes LE
// num_levels        : 4 bytes LE
//   per-level (num_levels times):
//     samples_per_peak  : 4 bytes LE
//     peaks_per_channel : 4 bytes LE
//       per-channel (num_channels times):
//         per-peak (peaks_per_channel times):
//           min_q7 : 1 byte  (int8)
//           max_q7 : 1 byte  (int8)
//
// Bumping `kThumbnailDiskVersion` invalidates every existing on-disk
// cache entry — that is the contract.

namespace {

constexpr char     kThumbnailMagic[4] = {'P', 'T', 'H', 'M'};
constexpr uint16_t kThumbnailDiskVersion = 1;

// Cap on a single .thumb blob we'll read from disk before allocating.
// Real thumbnails are small (a 30-minute stereo file at level0
// samples_per_peak=512 ≈ 30·60·44100·2/512 ≈ 620 KB; deeper LODs add a
// geometric tail). 64 MiB is ~100× the largest plausible thumbnail and
// safely below any 32-bit allocation panic.  Anything larger is treated
// as a corrupt/hostile cache entry: load_from_disk() returns nullptr and
// the cache silently behaves like a miss (rebuild from source), matching
// the documented contract. (Regression: #2966 / Codex 3305530560.)
constexpr std::streamsize kMaxThumbnailDiskBytes = 64 * 1024 * 1024;

template <typename T>
void append_le(std::vector<uint8_t>& out, T value) {
    static_assert(std::is_integral_v<T>, "append_le wants an integer");
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        out.push_back(static_cast<uint8_t>((value >> (8 * i)) & 0xFFu));
    }
}

template <typename T>
bool read_le(const uint8_t* data, std::size_t size, std::size_t& off, T& out) {
    if (off + sizeof(T) > size) return false;
    T v = 0;
    for (std::size_t i = 0; i < sizeof(T); ++i) {
        v = static_cast<T>(v | (static_cast<T>(data[off + i]) << (8 * i)));
    }
    off += sizeof(T);
    out = v;
    return true;
}

}  // namespace

std::vector<uint8_t> serialize_thumbnail(const AudioThumbnail& t) {
    const auto info = t.info();
    std::vector<uint8_t> out;
    // 4 (magic) + 2 (ver) + 4 + 8 + 4 + 4 + per-level overhead + peaks.
    out.reserve(26 + info.bytes_used + info.num_levels * 8);
    out.insert(out.end(),
               reinterpret_cast<const uint8_t*>(kThumbnailMagic),
               reinterpret_cast<const uint8_t*>(kThumbnailMagic) + 4);
    append_le<uint16_t>(out, kThumbnailDiskVersion);
    append_le<uint32_t>(out, info.num_channels);
    append_le<uint64_t>(out, info.num_source_frames);
    append_le<uint32_t>(out, info.sample_rate);
    append_le<uint32_t>(out, static_cast<uint32_t>(info.num_levels));
    for (std::size_t i = 0; i < info.num_levels; ++i) {
        const auto& lvl = t.level(i);
        append_le<uint32_t>(out, lvl.samples_per_peak);
        append_le<uint32_t>(out, lvl.peaks_per_channel);
        for (const auto& ch : lvl.peaks) {
            for (const auto& pk : ch) {
                out.push_back(static_cast<uint8_t>(pk.min_q7));
                out.push_back(static_cast<uint8_t>(pk.max_q7));
            }
        }
    }
    return out;
}

std::optional<AudioThumbnail> deserialize_thumbnail(const uint8_t* data,
                                                    std::size_t size) {
    if (data == nullptr || size < 26) return std::nullopt;
    if (std::memcmp(data, kThumbnailMagic, 4) != 0) return std::nullopt;
    std::size_t off = 4;
    uint16_t ver = 0;
    if (!read_le<uint16_t>(data, size, off, ver)) return std::nullopt;
    if (ver != kThumbnailDiskVersion) return std::nullopt;
    uint32_t num_channels = 0;
    uint64_t num_frames = 0;
    uint32_t sample_rate = 0;
    uint32_t num_levels = 0;
    if (!read_le<uint32_t>(data, size, off, num_channels)) return std::nullopt;
    if (!read_le<uint64_t>(data, size, off, num_frames)) return std::nullopt;
    if (!read_le<uint32_t>(data, size, off, sample_rate)) return std::nullopt;
    if (!read_le<uint32_t>(data, size, off, num_levels)) return std::nullopt;
    // Reasonable upper bounds so a corrupt header can't allocate a TB of RAM.
    if (num_channels > 64) return std::nullopt;
    if (num_levels   > 32) return std::nullopt;

    // Reconstruct via the same internal layout the in-memory build uses.
    // We friend nothing — every field is in the public surface (`info()`
    // for the read side, but here we need to populate from outside, so we
    // build through a temporary AudioFileData → build_from_buffer would
    // re-decimate; we want bit-identical levels. So we rebuild the levels
    // list manually using a friend-free path: a placement constructor via
    // a static factory inside AudioThumbnail would be cleaner, but
    // deserialize is rare and the levels are public-readable, so we wrap
    // the rebuild as a private helper via a friend declaration in the
    // header. The simplest no-friend approach: use the existing public
    // `levels_.push_back` is private. We add a small builder helper that
    // walks the wire and yields a thumbnail. We keep that helper free
    // and friend it in the header.
    //
    // Avoiding header churn: AudioThumbnail's data members are
    // accessible to functions inside the audio namespace via the
    // implementation file ONLY through public API. So we go through a
    // tiny accessor: load via build_from_buffer with a single-sample
    // stub and then mutate via friend. To keep this fully self-contained
    // we instead construct a synthetic AudioFileData that produces the
    // exact peak hierarchy — overkill. Cleanest pragmatic path: declare
    // an "internal" loader as a friend.
    //
    return AudioThumbnail::from_serialized_levels(
        num_channels, num_frames, sample_rate, num_levels,
        data, size, off);
}

std::optional<AudioThumbnail> AudioThumbnail::from_serialized_levels(
    uint32_t num_channels,
    uint64_t num_source_frames,
    uint32_t sample_rate,
    uint32_t num_levels,
    const uint8_t* data,
    std::size_t size,
    std::size_t offset) {
    AudioThumbnail t;
    t.num_channels_ = num_channels;
    t.num_source_frames_ = num_source_frames;
    t.sample_rate_ = sample_rate;
    t.levels_.reserve(num_levels);

    std::size_t off = offset;
    for (uint32_t i = 0; i < num_levels; ++i) {
        uint32_t samples_per_peak = 0;
        uint32_t peaks_per_channel = 0;
        if (!read_le<uint32_t>(data, size, off, samples_per_peak)) return std::nullopt;
        if (!read_le<uint32_t>(data, size, off, peaks_per_channel)) return std::nullopt;
        // Per-peak bytes: num_channels * peaks_per_channel * 2 (min,max).
        const std::size_t need = static_cast<std::size_t>(num_channels)
                               * static_cast<std::size_t>(peaks_per_channel)
                               * 2u;
        if (off + need > size) return std::nullopt;
        ThumbnailLevel lvl;
        lvl.samples_per_peak = samples_per_peak;
        lvl.peaks_per_channel = peaks_per_channel;
        lvl.peaks.resize(num_channels);
        for (uint32_t ch = 0; ch < num_channels; ++ch) {
            lvl.peaks[ch].resize(peaks_per_channel);
            for (uint32_t p = 0; p < peaks_per_channel; ++p) {
                AudioPeak pk;
                pk.min_q7 = static_cast<int8_t>(data[off++]);
                pk.max_q7 = static_cast<int8_t>(data[off++]);
                lvl.peaks[ch][p] = pk;
            }
        }
        t.levels_.push_back(std::move(lvl));
    }
    return t;
}

// ─────────────────────────────────────────────────────────────────────────
// AudioThumbnail
// ─────────────────────────────────────────────────────────────────────────

static ThumbnailLevel decimate_level(const ThumbnailLevel& src) {
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

AudioThumbnail AudioThumbnail::build_from_buffer(
    const AudioFileData& data,
    uint32_t samples_per_peak) {
    AudioThumbnail t;
    if (data.empty() || samples_per_peak == 0) {
        return t;
    }

    t.num_channels_ = data.num_channels();
    t.num_source_frames_ = data.num_frames();
    t.sample_rate_ = data.sample_rate;

    // Base level.
    ThumbnailLevel base;
    base.samples_per_peak = samples_per_peak;
    const uint64_t frames = data.num_frames();
    const uint32_t peaks =
        static_cast<uint32_t>((frames + samples_per_peak - 1u) / samples_per_peak);
    base.peaks_per_channel = peaks;
    base.peaks.resize(data.num_channels());

    for (std::size_t ch = 0; ch < data.num_channels(); ++ch) {
        const auto& src = data.channels[ch];
        auto& dst = base.peaks[ch];
        dst.resize(peaks);
        for (uint32_t p = 0; p < peaks; ++p) {
            const std::size_t a = static_cast<std::size_t>(p) * samples_per_peak;
            const std::size_t b = std::min<std::size_t>(a + samples_per_peak, src.size());
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

std::optional<AudioThumbnail> AudioThumbnail::build_from_path(
    const std::string& path,
    uint32_t samples_per_peak) {
    auto data = read_audio_file(path);
    if (!data) return std::nullopt;
    if (data->empty()) return std::nullopt;
    return build_from_buffer(*data, samples_per_peak);
}

AudioThumbnailInfo AudioThumbnail::info() const noexcept {
    AudioThumbnailInfo i;
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

std::size_t AudioThumbnail::best_level_for(uint32_t target_peaks) const noexcept {
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

std::size_t AudioThumbnail::render_min_max(uint32_t channel,
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
        // Map output index p ∈ [0, want) to a span over the source
        // [a, b) ⊂ [0, src_count) so we never sample past the end.
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

// ─────────────────────────────────────────────────────────────────────────
// AudioThumbnailCache
// ─────────────────────────────────────────────────────────────────────────

AudioThumbnailCache::AudioThumbnailCache(std::size_t max_entries)
    : capacity_(max_entries == 0 ? 1u : max_entries) {}

void AudioThumbnailCache::touch_locked(Map::iterator it) {
    list_.splice(list_.begin(), list_, it->second);
}

void AudioThumbnailCache::evict_to_capacity_locked() {
    while (list_.size() > capacity_) {
        const auto& victim = list_.back();
        index_.erase(victim.key);
        list_.pop_back();
        evictions_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::shared_ptr<const AudioThumbnail> AudioThumbnailCache::get(
    const std::string& path) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = index_.find(path);
    if (it == index_.end()) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    touch_locked(it);
    hits_.fetch_add(1, std::memory_order_relaxed);
    return it->second->value;
}

void AudioThumbnailCache::put(const std::string& path,
                               std::shared_ptr<const AudioThumbnail> thumbnail) {
    if (!thumbnail) return;
    // Snapshot disk-cache-dir under the lock to decide whether to write.
    std::string disk_dir_snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = index_.find(path);
        if (it != index_.end()) {
            it->second->value = thumbnail;
            touch_locked(it);
        } else {
            list_.push_front(Entry{path, thumbnail});
            index_[path] = list_.begin();
            evict_to_capacity_locked();
        }
        disk_dir_snapshot = disk_dir_;
    }
    if (!disk_dir_snapshot.empty()) {
        if (write_to_disk(path, *thumbnail)) {
            disk_writes_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

std::shared_ptr<const AudioThumbnail> AudioThumbnailCache::get_or_build(
    const std::string& path,
    uint32_t samples_per_peak) {
    if (auto cached = get(path)) return cached;
    // Disk lookup before we touch the audio decoder. This is the warm-
    // cache path across process restarts.
    if (auto from_disk = load_from_disk(path)) {
        disk_hits_.fetch_add(1, std::memory_order_relaxed);
        // Populate the in-memory cache for subsequent hits in this process.
        // Use the private insert path directly so we don't re-write the same
        // blob back to disk.
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = index_.find(path);
        if (it != index_.end()) {
            it->second->value = from_disk;
            touch_locked(it);
        } else {
            list_.push_front(Entry{path, from_disk});
            index_[path] = list_.begin();
            evict_to_capacity_locked();
        }
        return from_disk;
    }
    auto built = AudioThumbnail::build_from_path(path, samples_per_peak);
    if (!built) return nullptr;
    auto shared = std::make_shared<const AudioThumbnail>(std::move(*built));
    put(path, shared);
    return shared;
}

void AudioThumbnailCache::clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    list_.clear();
    index_.clear();
}

bool AudioThumbnailCache::erase(const std::string& path) {
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = index_.find(path);
    if (it == index_.end()) return false;
    list_.erase(it->second);
    index_.erase(it);
    return true;
}

AudioThumbnailCacheStats AudioThumbnailCache::stats() const {
    AudioThumbnailCacheStats s;
    s.hits = hits_.load(std::memory_order_relaxed);
    s.misses = misses_.load(std::memory_order_relaxed);
    s.evictions = evictions_.load(std::memory_order_relaxed);
    s.disk_hits = disk_hits_.load(std::memory_order_relaxed);
    s.disk_writes = disk_writes_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        s.entries = list_.size();
    }
    s.capacity = capacity_;
    return s;
}

std::size_t AudioThumbnailCache::size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return list_.size();
}

void AudioThumbnailCache::set_capacity(std::size_t max_entries) {
    std::lock_guard<std::mutex> lock(mtx_);
    capacity_ = max_entries == 0 ? 1u : max_entries;
    evict_to_capacity_locked();
}

// ─────────────────────────────────────────────────────────────────────────
// Disk-cache helpers
// ─────────────────────────────────────────────────────────────────────────

void AudioThumbnailCache::set_disk_cache_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mtx_);
    disk_dir_ = dir;
    if (disk_dir_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(disk_dir_, ec);
    // Best-effort — ignore create failures; subsequent reads/writes will
    // fail loudly through their own error paths.
}

void AudioThumbnailCache::clear_disk_cache() {
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        dir = disk_dir_;
    }
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec)) {
        if (entry.path().extension() == ".thumb") {
            std::filesystem::remove(entry.path(), ec);
        }
    }
}

std::string AudioThumbnailCache::disk_path_for(const std::string& source_path) const {
    // Key combines path + mtime so editing the underlying audio
    // invalidates any cached peak table. mtime fetch failure (file moved /
    // permission) silently falls back to "path only" — the cache may then
    // return stale data, but only for the lifetime of the entry on disk.
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(source_path, ec);
    std::string key = source_path;
    if (!ec) {
        const auto count =
            static_cast<long long>(mtime.time_since_epoch().count());
        key += '|';
        key += std::to_string(count);
    }
    const std::string digest = pulp::runtime::sha256_hex(key);
    // disk_dir_ is read here without the mutex; callers (load_from_disk /
    // write_to_disk) snapshot disk_dir_ before invoking us indirectly.
    // We re-read it under the lock for safety.
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        dir = disk_dir_;
    }
    if (dir.empty()) return {};
    std::filesystem::path p(dir);
    p /= digest + ".thumb";
    return p.string();
}

std::shared_ptr<const AudioThumbnail> AudioThumbnailCache::load_from_disk(
    const std::string& path) const {
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        dir = disk_dir_;
    }
    if (dir.empty()) return nullptr;
    const std::string disk_path = disk_path_for(path);
    if (disk_path.empty()) return nullptr;
    std::ifstream in(disk_path, std::ios::binary);
    if (!in.is_open()) return nullptr;
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    if (size <= 0) return nullptr;
    // Cap blob size BEFORE allocating. A corrupt or hostile .thumb file
    // claiming an absurd size would otherwise throw std::bad_alloc /
    // terminate when constructing `buf`. Treat oversize entries as a
    // cache miss so the caller silently rebuilds from source, matching
    // the documented "silently ignored" contract for bad cache entries.
    if (size > kMaxThumbnailDiskBytes) return nullptr;
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(buf.data()), size);
    if (!in) return nullptr;
    auto thumb = deserialize_thumbnail(buf.data(), buf.size());
    if (!thumb) return nullptr;
    return std::make_shared<const AudioThumbnail>(std::move(*thumb));
}

bool AudioThumbnailCache::write_to_disk(const std::string& path,
                                        const AudioThumbnail& thumbnail) const {
    const std::string disk_path = disk_path_for(path);
    if (disk_path.empty()) return false;
    const auto blob = serialize_thumbnail(thumbnail);
    // Write to a sibling tmp file and rename so a crash mid-write doesn't
    // leave a half-formed cache entry behind.
    const std::string tmp_path = disk_path + ".tmp";
    {
        std::ofstream out(tmp_path, std::ios::binary | std::ios::trunc);
        if (!out.is_open()) return false;
        out.write(reinterpret_cast<const char*>(blob.data()),
                  static_cast<std::streamsize>(blob.size()));
        if (!out) return false;
    }
    std::error_code ec;
    std::filesystem::rename(tmp_path, disk_path, ec);
    if (ec) {
        std::filesystem::remove(tmp_path, ec);
        return false;
    }
    return true;
}

}  // namespace pulp::audio
