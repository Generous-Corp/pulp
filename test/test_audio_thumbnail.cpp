// AudioThumbnail + AudioThumbnailCache tests.
// Pulp item 6.12 — wire AudioThumbnail into existing WaveformView.
//
// Coverage:
//   * peak-table generation against a known sine wave (min/max bounds,
//     decimation hierarchy, edge cases).
//   * AudioThumbnailCache hit/miss/eviction LRU semantics.
//   * Round-trip through `build_from_path` using a temp WAV file.
//   * WaveformView integration smoke: setting a thumbnail clears any
//     prior raw sample buffer; calling set_data clears any prior
//     thumbnail.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/audio/audio_file.hpp>
#include <pulp/audio/audio_thumbnail.hpp>
#include <pulp/view/widgets.hpp>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <numbers>
#include <string>
#include <vector>

using namespace pulp::audio;
using Catch::Matchers::WithinAbs;

namespace {

constexpr double kPi = std::numbers::pi_v<double>;

AudioFileData make_sine(uint32_t sample_rate,
                        uint64_t num_frames,
                        uint32_t num_channels,
                        double freq_hz,
                        float amplitude = 0.9f) {
    AudioFileData data;
    data.sample_rate = sample_rate;
    data.channels.resize(num_channels);
    for (auto& ch : data.channels) ch.resize(num_frames);

    const double phase_inc = 2.0 * kPi * freq_hz / static_cast<double>(sample_rate);
    for (uint64_t i = 0; i < num_frames; ++i) {
        const float s = static_cast<float>(std::sin(phase_inc * static_cast<double>(i))) * amplitude;
        for (uint32_t c = 0; c < num_channels; ++c) {
            data.channels[c][i] = s;
        }
    }
    return data;
}

std::filesystem::path unique_wav_path() {
    static int counter = 0;
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto stem = "pulp_test_thumbnail_"
              + std::to_string(reinterpret_cast<std::uintptr_t>(&counter))
              + "_" + std::to_string(tick)
              + "_" + std::to_string(counter++) + ".wav";
    return std::filesystem::temp_directory_path() / stem;
}

}  // namespace

TEST_CASE("AudioThumbnail empty buffer returns empty thumbnail", "[audio][thumbnail]") {
    AudioFileData empty;
    auto t = AudioThumbnail::build_from_buffer(empty);
    REQUIRE(t.empty());
    REQUIRE(t.num_levels() == 0);
}

TEST_CASE("AudioThumbnail peak-table covers the source amplitude", "[audio][thumbnail]") {
    // 1 s sine at 100 Hz, 44.1 kHz, mono, amplitude 0.5.
    const auto data = make_sine(44100, 44100, 1, 100.0, 0.5f);
    auto t = AudioThumbnail::build_from_buffer(data, /*samples_per_peak*/ 441);
    REQUIRE_FALSE(t.empty());

    const auto info = t.info();
    REQUIRE(info.num_channels == 1);
    REQUIRE(info.num_source_frames == 44100);
    REQUIRE(info.sample_rate == 44100);
    REQUIRE(info.num_levels >= 2);  // base + at least one decimation
    REQUIRE(info.bytes_used > 0);

    const auto& base = t.level(0);
    REQUIRE(base.samples_per_peak == 441);
    REQUIRE(base.peaks_per_channel == 100);
    REQUIRE(base.peaks.size() == 1);

    // Most slots span more than one full sine period (441 samples at
    // 100 Hz / 44.1 kHz is ~1 period); expect a near-±0.5 envelope.
    int saturated = 0;
    for (const auto& pk : base.peaks[0]) {
        REQUIRE(pk.min() >= -1.0f);
        REQUIRE(pk.max() <=  1.0f);
        REQUIRE(pk.min() <= 0.0f);
        REQUIRE(pk.max() >= 0.0f);
        if (pk.max() > 0.45f && pk.min() < -0.45f) saturated++;
    }
    // The vast majority of slots should hit the full envelope.
    REQUIRE(saturated >= 80);
}

TEST_CASE("AudioThumbnail decimation halves peak count per level", "[audio][thumbnail]") {
    const auto data = make_sine(44100, 44100, 2, 200.0);
    auto t = AudioThumbnail::build_from_buffer(data, 256);
    REQUIRE(t.num_levels() >= 2);

    for (std::size_t i = 1; i < t.num_levels(); ++i) {
        const auto& prev = t.level(i - 1);
        const auto& cur  = t.level(i);
        REQUIRE(cur.samples_per_peak == prev.samples_per_peak * 2u);
        REQUIRE(cur.peaks_per_channel == (prev.peaks_per_channel + 1u) / 2u);
    }
    const auto& finest = t.level(t.num_levels() - 1);
    REQUIRE(finest.peaks_per_channel <= 16u);
}

TEST_CASE("AudioThumbnail::render_min_max produces bounded peak pairs", "[audio][thumbnail]") {
    const auto data = make_sine(48000, 48000, 2, 250.0, 0.8f);
    auto t = AudioThumbnail::build_from_buffer(data, 128);

    constexpr uint32_t kTargetPeaks = 200;
    std::vector<float> out(kTargetPeaks * 2);
    const std::size_t produced =
        t.render_min_max(AudioThumbnail::kAllChannels, kTargetPeaks, out.data());
    REQUIRE(produced == kTargetPeaks);

    for (std::size_t i = 0; i < kTargetPeaks; ++i) {
        const float lo = out[i * 2];
        const float hi = out[i * 2 + 1];
        REQUIRE(lo >= -1.0f);
        REQUIRE(hi <=  1.0f);
        REQUIRE(lo <= hi);
    }

    // Picking a single channel works without folding.
    std::vector<float> ch0(kTargetPeaks * 2);
    REQUIRE(t.render_min_max(0, kTargetPeaks, ch0.data()) == kTargetPeaks);
    std::vector<float> ch1(kTargetPeaks * 2);
    REQUIRE(t.render_min_max(1, kTargetPeaks, ch1.data()) == kTargetPeaks);
}

TEST_CASE("AudioThumbnail::best_level_for picks the coarsest sufficient level", "[audio][thumbnail]") {
    const auto data = make_sine(44100, 44100, 1, 100.0);
    auto t = AudioThumbnail::build_from_buffer(data, 256);
    REQUIRE(t.num_levels() >= 2);

    // Asking for more peaks than the base level should pin level 0.
    const auto base = t.level(0).peaks_per_channel;
    REQUIRE(t.best_level_for(base + 100u) == 0);

    // Asking for very few peaks should pick the coarsest level.
    REQUIRE(t.best_level_for(1) == t.num_levels() - 1);
}

TEST_CASE("AudioThumbnail::build_from_path round-trips through a WAV", "[audio][thumbnail]") {
    const auto path = unique_wav_path();
    const auto data = make_sine(44100, 22050, 1, 440.0, 0.7f);
    REQUIRE(write_wav_file(path.string(), data));

    auto t = AudioThumbnail::build_from_path(path.string(), 128);
    REQUIRE(t.has_value());
    REQUIRE_FALSE(t->empty());
    REQUIRE(t->info().num_source_frames == 22050);
    REQUIRE(t->info().num_channels == 1);

    std::filesystem::remove(path);
}

TEST_CASE("AudioThumbnail::build_from_path returns nullopt for missing file", "[audio][thumbnail]") {
    auto t = AudioThumbnail::build_from_path("/definitely/does/not/exist.wav");
    REQUIRE_FALSE(t.has_value());
}

// ─────────────────────────────────────────────────────────────────────────
// AudioThumbnailCache
// ─────────────────────────────────────────────────────────────────────────

TEST_CASE("AudioThumbnailCache records hit / miss statistics", "[audio][thumbnail][cache]") {
    AudioThumbnailCache cache(4);
    REQUIRE(cache.size() == 0);

    REQUIRE(cache.get("absent") == nullptr);
    auto stats = cache.stats();
    REQUIRE(stats.misses == 1);
    REQUIRE(stats.hits == 0);

    auto t = std::make_shared<AudioThumbnail>(
        AudioThumbnail::build_from_buffer(make_sine(44100, 4410, 1, 200.0), 256));
    cache.put("/tmp/a.wav", t);

    auto fetched = cache.get("/tmp/a.wav");
    REQUIRE(fetched.get() == t.get());
    stats = cache.stats();
    REQUIRE(stats.hits == 1);
    REQUIRE(stats.misses == 1);
    REQUIRE(stats.entries == 1);
}

TEST_CASE("AudioThumbnailCache LRU evicts the oldest entry", "[audio][thumbnail][cache]") {
    AudioThumbnailCache cache(2);

    auto a = std::make_shared<AudioThumbnail>(
        AudioThumbnail::build_from_buffer(make_sine(44100, 4410, 1, 100.0), 256));
    auto b = std::make_shared<AudioThumbnail>(
        AudioThumbnail::build_from_buffer(make_sine(44100, 4410, 1, 200.0), 256));
    auto c = std::make_shared<AudioThumbnail>(
        AudioThumbnail::build_from_buffer(make_sine(44100, 4410, 1, 400.0), 256));

    cache.put("a", a);
    cache.put("b", b);
    REQUIRE(cache.size() == 2);

    // Touch a so b becomes the LRU.
    REQUIRE(cache.get("a") != nullptr);

    cache.put("c", c);
    REQUIRE(cache.size() == 2);

    // b should have been evicted; a + c remain.
    REQUIRE(cache.get("a") != nullptr);
    REQUIRE(cache.get("c") != nullptr);
    REQUIRE(cache.get("b") == nullptr);

    const auto s = cache.stats();
    REQUIRE(s.evictions == 1);
    REQUIRE(s.entries == 2);
    REQUIRE(s.capacity == 2);
}

TEST_CASE("AudioThumbnailCache::set_capacity shrinks the cache", "[audio][thumbnail][cache]") {
    AudioThumbnailCache cache(4);
    auto t = std::make_shared<AudioThumbnail>(
        AudioThumbnail::build_from_buffer(make_sine(44100, 4410, 1, 100.0), 256));
    cache.put("a", t);
    cache.put("b", t);
    cache.put("c", t);
    cache.put("d", t);
    REQUIRE(cache.size() == 4);

    cache.set_capacity(2);
    REQUIRE(cache.size() == 2);
    REQUIRE(cache.stats().evictions == 2);
}

TEST_CASE("AudioThumbnailCache::erase + clear", "[audio][thumbnail][cache]") {
    AudioThumbnailCache cache(4);
    auto t = std::make_shared<AudioThumbnail>(
        AudioThumbnail::build_from_buffer(make_sine(44100, 4410, 1, 100.0), 256));
    cache.put("a", t);
    cache.put("b", t);
    REQUIRE(cache.erase("a"));
    REQUIRE_FALSE(cache.erase("nonexistent"));
    REQUIRE(cache.size() == 1);
    cache.clear();
    REQUIRE(cache.size() == 0);
}

TEST_CASE("AudioThumbnailCache::get_or_build hits the cache on second call",
          "[audio][thumbnail][cache]") {
    const auto path = unique_wav_path();
    const auto data = make_sine(44100, 22050, 1, 440.0, 0.7f);
    REQUIRE(write_wav_file(path.string(), data));

    AudioThumbnailCache cache(4);
    auto first = cache.get_or_build(path.string(), 256);
    REQUIRE(first != nullptr);
    auto second = cache.get_or_build(path.string(), 256);
    REQUIRE(second != nullptr);
    REQUIRE(first.get() == second.get());  // shared instance => cache hit

    std::filesystem::remove(path);
}

// ─────────────────────────────────────────────────────────────────────────
// WaveformView integration smoke
// ─────────────────────────────────────────────────────────────────────────

// ─────────────────────────────────────────────────────────────────────────
// On-disk persistence (item 6.12 follow-up)
// ─────────────────────────────────────────────────────────────────────────

namespace {

std::filesystem::path unique_cache_dir() {
    static int counter = 0;
    const auto tick = std::chrono::steady_clock::now().time_since_epoch().count();
    auto name = "pulp_test_thumbcache_"
              + std::to_string(reinterpret_cast<std::uintptr_t>(&counter))
              + "_" + std::to_string(tick)
              + "_" + std::to_string(counter++);
    auto p = std::filesystem::temp_directory_path() / name;
    std::error_code ec;
    std::filesystem::remove_all(p, ec);
    return p;
}

}  // namespace

TEST_CASE("serialize_thumbnail round-trips through deserialize_thumbnail",
          "[audio][thumbnail][persist]") {
    const auto data = make_sine(44100, 8820, 2, 220.0, 0.6f);
    AudioThumbnail original = AudioThumbnail::build_from_buffer(data, 256);
    REQUIRE_FALSE(original.empty());

    const auto blob = serialize_thumbnail(original);
    REQUIRE(blob.size() > 26);  // header + payload
    REQUIRE(blob[0] == 'P');
    REQUIRE(blob[1] == 'T');
    REQUIRE(blob[2] == 'H');
    REQUIRE(blob[3] == 'M');

    auto restored = deserialize_thumbnail(blob.data(), blob.size());
    REQUIRE(restored.has_value());
    REQUIRE_FALSE(restored->empty());

    const auto a = original.info();
    const auto b = restored->info();
    REQUIRE(a.num_channels == b.num_channels);
    REQUIRE(a.num_source_frames == b.num_source_frames);
    REQUIRE(a.sample_rate == b.sample_rate);
    REQUIRE(a.num_levels == b.num_levels);
    REQUIRE(a.bytes_used == b.bytes_used);

    // Spot-check every peak in level 0 is identical post round-trip.
    for (std::size_t i = 0; i < original.num_levels(); ++i) {
        const auto& la = original.level(i);
        const auto& lb = restored->level(i);
        REQUIRE(la.samples_per_peak == lb.samples_per_peak);
        REQUIRE(la.peaks_per_channel == lb.peaks_per_channel);
        REQUIRE(la.peaks.size() == lb.peaks.size());
        for (std::size_t ch = 0; ch < la.peaks.size(); ++ch) {
            REQUIRE(la.peaks[ch].size() == lb.peaks[ch].size());
            for (std::size_t p = 0; p < la.peaks[ch].size(); ++p) {
                REQUIRE(la.peaks[ch][p].min_q7 == lb.peaks[ch][p].min_q7);
                REQUIRE(la.peaks[ch][p].max_q7 == lb.peaks[ch][p].max_q7);
            }
        }
    }
}

TEST_CASE("deserialize_thumbnail rejects bad magic / truncated blobs",
          "[audio][thumbnail][persist]") {
    std::vector<uint8_t> garbage{0xDE, 0xAD, 0xBE, 0xEF,
                                  0x01, 0x00, 0x00, 0x00};
    REQUIRE_FALSE(deserialize_thumbnail(garbage.data(), garbage.size()).has_value());
    REQUIRE_FALSE(deserialize_thumbnail(nullptr, 0).has_value());

    // Correct magic + bumped version → still rejected.
    std::vector<uint8_t> wrong_version{'P', 'T', 'H', 'M',
                                       0xFF, 0xFF};
    REQUIRE_FALSE(deserialize_thumbnail(wrong_version.data(),
                                        wrong_version.size()).has_value());
}

TEST_CASE("AudioThumbnailCache writes and loads from disk across instances",
          "[audio][thumbnail][cache][persist]") {
    const auto cache_dir = unique_cache_dir();
    const auto wav_path = unique_wav_path();
    const auto data = make_sine(44100, 22050, 1, 330.0, 0.5f);
    REQUIRE(write_wav_file(wav_path.string(), data));

    // Pass 1 — build, expect a disk write.
    {
        AudioThumbnailCache cache(4);
        cache.set_disk_cache_dir(cache_dir.string());
        auto t = cache.get_or_build(wav_path.string(), 256);
        REQUIRE(t != nullptr);
        const auto s = cache.stats();
        REQUIRE(s.disk_writes == 1);
        REQUIRE(s.disk_hits == 0);
    }

    // Pass 2 — fresh cache, expect a disk hit and no extra decode.
    {
        AudioThumbnailCache cache(4);
        cache.set_disk_cache_dir(cache_dir.string());
        auto t = cache.get_or_build(wav_path.string(), 256);
        REQUIRE(t != nullptr);
        const auto s = cache.stats();
        REQUIRE(s.disk_hits == 1);
        REQUIRE(s.entries == 1);
        REQUIRE(s.misses == 1);  // initial in-memory miss before disk lookup
    }

    // Clean up.
    std::filesystem::remove(wav_path);
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
}

TEST_CASE("AudioThumbnailCache::set_disk_cache_dir is a no-op when empty",
          "[audio][thumbnail][cache][persist]") {
    AudioThumbnailCache cache(2);
    cache.set_disk_cache_dir("");
    REQUIRE(cache.disk_cache_dir().empty());
    auto t = std::make_shared<AudioThumbnail>(
        AudioThumbnail::build_from_buffer(make_sine(44100, 4410, 1, 200.0), 256));
    cache.put("no-disk", t);
    REQUIRE(cache.stats().disk_writes == 0);
}

TEST_CASE("AudioThumbnailCache::clear_disk_cache removes .thumb files",
          "[audio][thumbnail][cache][persist]") {
    const auto cache_dir = unique_cache_dir();
    const auto wav_path = unique_wav_path();
    REQUIRE(write_wav_file(wav_path.string(),
                           make_sine(44100, 4410, 1, 100.0)));

    AudioThumbnailCache cache(4);
    cache.set_disk_cache_dir(cache_dir.string());
    REQUIRE(cache.get_or_build(wav_path.string(), 256) != nullptr);
    REQUIRE(cache.stats().disk_writes == 1);

    std::size_t thumb_files = 0;
    for (auto& entry : std::filesystem::directory_iterator(cache_dir)) {
        if (entry.path().extension() == ".thumb") thumb_files++;
    }
    REQUIRE(thumb_files == 1);

    cache.clear_disk_cache();
    thumb_files = 0;
    for (auto& entry : std::filesystem::directory_iterator(cache_dir)) {
        if (entry.path().extension() == ".thumb") thumb_files++;
    }
    REQUIRE(thumb_files == 0);

    std::filesystem::remove(wav_path);
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
}

// Regression: #2966 / Codex comment 3305530564 — clear_disk_cache() is
// documented as best-effort/no-op but the range-for over
// `directory_iterator(dir, ec)` still used the THROWING increment path,
// so a permission error or concurrent filesystem change mid-iteration
// could escape as filesystem_error. We pin REQUIRE_NOTHROW for the
// happy path (empty dir, missing dir, nonexistent dir) and for an
// extra-file-mid-iteration case.
TEST_CASE("AudioThumbnailCache::clear_disk_cache is non-throwing",
          "[audio][thumbnail][cache][persist][issue-2966]") {
    AudioThumbnailCache cache(2);

    // Case 1: no disk dir set — early return, no throw.
    REQUIRE_NOTHROW(cache.clear_disk_cache());

    // Case 2: dir is set but does not exist — early return, no throw.
    cache.set_disk_cache_dir("/this/path/definitely/does/not/exist/pulp-test");
    REQUIRE_NOTHROW(cache.clear_disk_cache());

    // Case 3: dir exists but is empty — iterates zero times, no throw.
    const auto empty_dir = unique_cache_dir();
    std::filesystem::create_directories(empty_dir);
    cache.set_disk_cache_dir(empty_dir.string());
    REQUIRE_NOTHROW(cache.clear_disk_cache());

    // Case 4: dir has a mix of .thumb files and unrelated files — should
    // clean only the .thumb files and never throw on the unrelated ones.
    {
        std::ofstream(empty_dir / "a.thumb").put('x');
        std::ofstream(empty_dir / "b.thumb").put('x');
        std::ofstream(empty_dir / "notes.txt").put('x');
        REQUIRE_NOTHROW(cache.clear_disk_cache());
        REQUIRE_FALSE(std::filesystem::exists(empty_dir / "a.thumb"));
        REQUIRE_FALSE(std::filesystem::exists(empty_dir / "b.thumb"));
        REQUIRE(std::filesystem::exists(empty_dir / "notes.txt"));
    }

    std::error_code ec;
    std::filesystem::remove_all(empty_dir, ec);
}

// Regression: #2966 / Codex comment 3305530560 — load_from_disk() used to
// trust the .thumb file size and allocate a `std::vector<uint8_t>` of that
// exact size with no upper bound. A corrupt or hostile oversized cache
// file would throw `std::bad_alloc` (or trip OOM-killer) instead of being
// treated as a cache miss. We pin the contract: an oversize blob in the
// cache directory must NOT crash get_or_build() and the call must fall
// back to rebuilding from source.
TEST_CASE("AudioThumbnailCache load_from_disk treats oversize blob as cache miss",
          "[audio][thumbnail][cache][persist][issue-2966]") {
    const auto cache_dir = unique_cache_dir();
    const auto wav_path = unique_wav_path();
    REQUIRE(write_wav_file(wav_path.string(),
                           make_sine(44100, 4410, 1, 220.0)));

    // Pass 1 — build a legitimate cache entry so we know the .thumb path.
    std::filesystem::path thumb_file;
    {
        AudioThumbnailCache cache(4);
        cache.set_disk_cache_dir(cache_dir.string());
        REQUIRE(cache.get_or_build(wav_path.string(), 256) != nullptr);
        REQUIRE(cache.stats().disk_writes == 1);

        for (auto& entry : std::filesystem::directory_iterator(cache_dir)) {
            if (entry.path().extension() == ".thumb") {
                thumb_file = entry.path();
                break;
            }
        }
        REQUIRE_FALSE(thumb_file.empty());
    }

    // Replace the real .thumb file with an oversize (~128 MiB) sparse file.
    // The cap in load_from_disk() (kMaxThumbnailDiskBytes = 64 MiB) must
    // reject it before any allocation is attempted.
    {
        std::error_code ec;
        std::filesystem::remove(thumb_file, ec);
        std::ofstream out(thumb_file, std::ios::binary | std::ios::trunc);
        REQUIRE(out.is_open());
        constexpr std::streamoff kOversize = 128LL * 1024 * 1024;  // 128 MiB
        out.seekp(kOversize - 1);
        out.put('\0');
        out.close();
        // Sanity: file is actually that size on disk (sparse or not).
        REQUIRE(std::filesystem::file_size(thumb_file) ==
                static_cast<std::uintmax_t>(kOversize));
    }

    // Pass 2 — fresh cache, must not crash or throw.
    {
        AudioThumbnailCache cache(4);
        cache.set_disk_cache_dir(cache_dir.string());
        std::shared_ptr<const AudioThumbnail> t;
        REQUIRE_NOTHROW(t = cache.get_or_build(wav_path.string(), 256));
        // Either the entry is silently treated as a miss and rebuilt from
        // source (preferred) or skipped entirely — both are acceptable;
        // the only forbidden outcome is bad_alloc / termination.
        REQUIRE(t != nullptr);
        // It must NOT have counted as a disk hit.
        REQUIRE(cache.stats().disk_hits == 0);
    }

    std::filesystem::remove(wav_path);
    std::error_code ec;
    std::filesystem::remove_all(cache_dir, ec);
}

TEST_CASE("WaveformView accepts an AudioThumbnail source", "[view][waveform][thumbnail]") {
    pulp::view::WaveformView view;
    REQUIRE_FALSE(view.has_thumbnail());

    auto data = make_sine(44100, 4410, 1, 200.0);
    AudioThumbnail thumb = AudioThumbnail::build_from_buffer(data, 256);
    REQUIRE_FALSE(thumb.empty());

    view.set_thumbnail(&thumb);
    REQUIRE(view.has_thumbnail());
    REQUIRE(view.thumbnail() == &thumb);
    // Setting a thumbnail clears any raw sample buffer.
    REQUIRE(view.sample_count() == 0);

    // Raw samples take precedence and disable the thumbnail.
    std::vector<float> raw(128, 0.1f);
    view.set_data(raw);
    REQUIRE_FALSE(view.has_thumbnail());
    REQUIRE(view.sample_count() == 128);

    // clear_thumbnail() is a no-op once samples are active.
    view.clear_thumbnail();
    REQUIRE_FALSE(view.has_thumbnail());
}
