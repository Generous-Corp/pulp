#include <pulp/audio/waveform_overview.hpp>

#include <pulp/runtime/crypto.hpp>

#include <filesystem>
#include <fstream>
#include <system_error>
#include <utility>
#include <vector>

namespace pulp::audio {

namespace {

// Cap on a single .thumb blob we'll read from disk before allocating.
// Real thumbnails are small; 64 MiB is intentionally far above plausible
// waveform summaries and below any accidental huge allocation. Anything
// larger is treated as a corrupt cache entry and rebuilt from source.
constexpr std::streamsize kMaxThumbnailDiskBytes = 64 * 1024 * 1024;

std::string memory_cache_key(const std::string& source_path,
                             uint32_t samples_per_peak) {
    return source_path + "|spp=" + std::to_string(samples_per_peak);
}

uint32_t thumbnail_base_samples_per_peak(const WaveformOverview& thumbnail,
                                         uint32_t fallback) {
    if (!thumbnail.empty() && thumbnail.num_levels() > 0) {
        const auto samples_per_peak = thumbnail.level(0).samples_per_peak;
        if (samples_per_peak != 0) return samples_per_peak;
    }
    return fallback == 0 ? 256u : fallback;
}

}  // namespace

WaveformOverviewCache::WaveformOverviewCache(std::size_t max_entries)
    : capacity_(max_entries == 0 ? 1u : max_entries) {}

void WaveformOverviewCache::touch_locked(Map::iterator it) {
    list_.splice(list_.begin(), list_, it->second);
}

void WaveformOverviewCache::evict_to_capacity_locked() {
    while (list_.size() > capacity_) {
        const auto& victim = list_.back();
        index_.erase(victim.key);
        list_.pop_back();
        evictions_.fetch_add(1, std::memory_order_relaxed);
    }
}

std::shared_ptr<const WaveformOverview> WaveformOverviewCache::get(
    const std::string& path,
    uint32_t samples_per_peak) {
    const auto key = memory_cache_key(path, samples_per_peak);
    std::lock_guard<std::mutex> lock(mtx_);
    auto it = index_.find(key);
    if (it == index_.end()) {
        misses_.fetch_add(1, std::memory_order_relaxed);
        return nullptr;
    }
    touch_locked(it);
    hits_.fetch_add(1, std::memory_order_relaxed);
    return it->second->value;
}

void WaveformOverviewCache::put(const std::string& path,
                              std::shared_ptr<const WaveformOverview> thumbnail,
                              uint32_t fallback_samples_per_peak) {
    if (!thumbnail) return;
    const auto resolved_samples_per_peak =
        thumbnail_base_samples_per_peak(*thumbnail, fallback_samples_per_peak);
    const auto key = memory_cache_key(path, resolved_samples_per_peak);
    // Snapshot disk-cache-dir under the lock to decide whether to write.
    std::string disk_dir_snapshot;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->value = thumbnail;
            touch_locked(it);
        } else {
            list_.push_front(Entry{key, thumbnail});
            index_[key] = list_.begin();
            evict_to_capacity_locked();
        }
        disk_dir_snapshot = disk_dir_;
    }
    if (!disk_dir_snapshot.empty()) {
        if (write_to_disk(path, resolved_samples_per_peak, *thumbnail)) {
            disk_writes_.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

std::shared_ptr<const WaveformOverview> WaveformOverviewCache::get_or_build(
    const std::string& path,
    uint32_t samples_per_peak) {
    if (auto cached = get(path, samples_per_peak)) return cached;
    // Disk lookup before we touch the audio decoder. This is the warm-
    // cache path across process restarts.
    const auto key = memory_cache_key(path, samples_per_peak);
    if (auto from_disk = load_from_disk(path, samples_per_peak)) {
        disk_hits_.fetch_add(1, std::memory_order_relaxed);
        // Populate the in-memory cache for subsequent hits in this process.
        // Use the private insert path directly so we don't re-write the same
        // blob back to disk.
        std::lock_guard<std::mutex> lock(mtx_);
        auto it = index_.find(key);
        if (it != index_.end()) {
            it->second->value = from_disk;
            touch_locked(it);
        } else {
            list_.push_front(Entry{key, from_disk});
            index_[key] = list_.begin();
            evict_to_capacity_locked();
        }
        return from_disk;
    }
    auto built = WaveformOverview::build_from_path(path, samples_per_peak);
    if (!built) return nullptr;
    auto shared = std::make_shared<const WaveformOverview>(std::move(*built));
    put(path, shared, samples_per_peak);
    return shared;
}

void WaveformOverviewCache::clear() {
    std::lock_guard<std::mutex> lock(mtx_);
    list_.clear();
    index_.clear();
}

bool WaveformOverviewCache::erase(const std::string& path) {
    std::lock_guard<std::mutex> lock(mtx_);
    const auto prefix = path + "|spp=";
    bool erased = false;
    for (auto it = list_.begin(); it != list_.end();) {
        if (it->key == path || it->key.rfind(prefix, 0) == 0) {
            index_.erase(it->key);
            it = list_.erase(it);
            erased = true;
        } else {
            ++it;
        }
    }
    return erased;
}

WaveformOverviewCacheStats WaveformOverviewCache::stats() const {
    WaveformOverviewCacheStats s;
    s.hits = hits_.load(std::memory_order_relaxed);
    s.misses = misses_.load(std::memory_order_relaxed);
    s.evictions = evictions_.load(std::memory_order_relaxed);
    s.disk_hits = disk_hits_.load(std::memory_order_relaxed);
    s.disk_writes = disk_writes_.load(std::memory_order_relaxed);
    {
        std::lock_guard<std::mutex> lock(mtx_);
        s.entries = list_.size();
        s.capacity = capacity_;
    }
    return s;
}

std::size_t WaveformOverviewCache::size() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return list_.size();
}

std::size_t WaveformOverviewCache::capacity() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return capacity_;
}

void WaveformOverviewCache::set_capacity(std::size_t max_entries) {
    std::lock_guard<std::mutex> lock(mtx_);
    capacity_ = max_entries == 0 ? 1u : max_entries;
    evict_to_capacity_locked();
}

void WaveformOverviewCache::set_disk_cache_dir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(mtx_);
    disk_dir_ = dir;
    if (disk_dir_.empty()) return;
    std::error_code ec;
    std::filesystem::create_directories(disk_dir_, ec);
    // Best-effort: ignore create failures; subsequent reads/writes fail
    // through their own error paths.
}

std::string WaveformOverviewCache::disk_cache_dir() const {
    std::lock_guard<std::mutex> lock(mtx_);
    return disk_dir_;
}

void WaveformOverviewCache::clear_disk_cache() {
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        dir = disk_dir_;
    }
    if (dir.empty()) return;
    std::error_code ec;
    if (!std::filesystem::is_directory(dir, ec)) return;
    // Documented contract: best-effort, never throws. A range-for over
    // `std::filesystem::directory_iterator(dir, ec)` would still use the
    // throwing increment path internally, so drive the iterator manually.
    auto it = std::filesystem::directory_iterator(dir, ec);
    const auto end = std::filesystem::directory_iterator{};
    while (!ec && it != end) {
        const auto& entry = *it;
        if (entry.path().extension() == ".thumb") {
            std::error_code rm_ec;
            std::filesystem::remove(entry.path(), rm_ec);
        }
        it.increment(ec);
    }
}

std::string WaveformOverviewCache::disk_path_for(const std::string& source_path,
                                               uint32_t samples_per_peak) const {
    // Key combines path + mtime so editing the underlying audio invalidates
    // cached peaks. mtime failure falls back to path-only keying.
    std::error_code ec;
    auto mtime = std::filesystem::last_write_time(source_path, ec);
    std::string key = source_path;
    if (!ec) {
        const auto count =
            static_cast<long long>(mtime.time_since_epoch().count());
        key += '|';
        key += std::to_string(count);
    }
    key += "|spp=";
    key += std::to_string(samples_per_peak);
    const std::string digest = pulp::runtime::sha256_hex(key);

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

std::shared_ptr<const WaveformOverview> WaveformOverviewCache::load_from_disk(
    const std::string& path,
    uint32_t samples_per_peak) const {
    std::string dir;
    {
        std::lock_guard<std::mutex> lock(mtx_);
        dir = disk_dir_;
    }
    if (dir.empty()) return nullptr;
    const std::string disk_path = disk_path_for(path, samples_per_peak);
    if (disk_path.empty()) return nullptr;
    std::ifstream in(disk_path, std::ios::binary);
    if (!in.is_open()) return nullptr;
    in.seekg(0, std::ios::end);
    const std::streamsize size = in.tellg();
    if (size <= 0) return nullptr;
    if (size > kMaxThumbnailDiskBytes) return nullptr;
    in.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf(static_cast<std::size_t>(size));
    in.read(reinterpret_cast<char*>(buf.data()), size);
    if (!in) return nullptr;
    auto thumb = deserialize_thumbnail(buf.data(), buf.size());
    if (!thumb) return nullptr;
    if (thumb->empty()) return nullptr;
    if (thumb->level(0).samples_per_peak != samples_per_peak) return nullptr;
    return std::make_shared<const WaveformOverview>(std::move(*thumb));
}

bool WaveformOverviewCache::write_to_disk(const std::string& path,
                                        uint32_t samples_per_peak,
                                        const WaveformOverview& thumbnail) const {
    const std::string disk_path = disk_path_for(path, samples_per_peak);
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
