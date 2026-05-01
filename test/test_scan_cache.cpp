// Unit tests for HostScanCache (workstream 03 slice 3.1).

#include <catch2/catch_test_macros.hpp>
#include <pulp/host/scan_cache.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <atomic>
#include <thread>

#if defined(_WIN32)
#include <process.h>
#else
#include <unistd.h>
#endif

using namespace pulp::host;
namespace fs = std::filesystem;

namespace {

int current_process_id() {
#if defined(_WIN32)
    return _getpid();
#else
    return getpid();
#endif
}

struct TempFile {
    fs::path path;
    TempFile() {
        static std::atomic<uint64_t> next_id{0};
        auto stem = "pulp-scan-cache-" + std::to_string(current_process_id()) + "-" +
                    std::to_string(std::chrono::steady_clock::now()
                                       .time_since_epoch()
                                       .count()) +
                    "-" + std::to_string(next_id.fetch_add(1));
        path = fs::temp_directory_path() / stem;
    }
    ~TempFile() { std::error_code ec; fs::remove_all(path, ec); }
    void write(const std::string& content) {
        std::ofstream(path, std::ios::binary) << content;
    }
};

PluginInfo sample_info() {
    PluginInfo p;
    p.name = "Delay";
    p.manufacturer = "Pulp";
    p.version = "1.2.3";
    p.path = "/plugins/Delay.vst3";
    p.unique_id = "com.pulp.delay";
    p.format = PluginFormat::VST3;
    p.is_instrument = false;
    p.is_effect = true;
    p.num_inputs = 2;
    p.num_outputs = 2;
    return p;
}

} // namespace

TEST_CASE("get returns nullopt for missing path", "[scan_cache]") {
    HostScanCache cache;
    REQUIRE(!cache.get("/does/not/exist.vst3").has_value());
}

TEST_CASE("put + get round-trip on an existing file", "[scan_cache]") {
    TempFile f;
    f.write("binary payload");
    HostScanCache cache;
    auto info = sample_info();
    info.path = f.path.string();
    cache.put(f.path.string(), info);
    REQUIRE(cache.size() == 1);
    auto got = cache.get(f.path.string());
    REQUIRE(got.has_value());
    REQUIRE(got->name == "Delay");
    REQUIRE(got->format == PluginFormat::VST3);
    REQUIRE(got->unique_id == "com.pulp.delay");
}

TEST_CASE("get invalidates when file changes", "[scan_cache]") {
    TempFile f;
    f.write("original");
    HostScanCache cache;
    cache.put(f.path.string(), sample_info());
    REQUIRE(cache.get(f.path.string()).has_value());
    // Sleep past mtime granularity (ext4/APFS: 1s). 1100ms is defensive.
    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    f.write("rewritten with different length");
    REQUIRE(!cache.get(f.path.string()).has_value());
}

TEST_CASE("JSON round-trip preserves all fields", "[scan_cache]") {
    HostScanCache a;
    a.put("/tmp/nonexistent-but-ok.vst3", sample_info());
    auto json = a.to_json();
    REQUIRE(json.find("schema_version") != std::string::npos);
    REQUIRE(json.find("Delay") != std::string::npos);

    HostScanCache b;
    REQUIRE(b.from_json(json));
    REQUIRE(b.size() == 1);
    const auto& entries = b.entries();
    REQUIRE(entries.count("/tmp/nonexistent-but-ok.vst3") == 1);
    const auto& e = entries.at("/tmp/nonexistent-but-ok.vst3");
    REQUIRE(e.info.manufacturer == "Pulp");
    REQUIRE(e.info.format == PluginFormat::VST3);
}

TEST_CASE("JSON round-trip preserves workstream 03 slice 3.7 metadata",
          "[scan_cache][metadata]") {
    // Covers the additive richer-metadata fields on PluginInfo —
    // scan_cache.hpp's doc promises "older cache blobs that don't
    // carry them deserialize with defaults". This case pins that
    // promise when the fields *are* carried, complementing the
    // default-init verification in test_plugin_info_metadata.cpp.
    HostScanCache a;
    PluginInfo rich;
    rich.name = "SpectrumMeter";
    rich.manufacturer = "Pulp";
    rich.version = "2.0.0";
    rich.path = "/tmp/rich-plugin.clap";
    rich.unique_id = "com.pulp.spectrum";
    rich.format = PluginFormat::CLAP;
    rich.is_instrument = false;
    rich.is_effect = true;
    rich.num_inputs = 2;
    rich.num_outputs = 2;
    rich.category = "Analyzer";
    rich.features = {"audio-effect", "analyzer", "utility"};
    rich.description = "Real-time spectrum + phase analyzer.";
    rich.has_editor = true;
    rich.supports_sidechain = true;
    rich.supports_midi_in = false;
    rich.supports_midi_out = false;

    a.put("/tmp/rich-plugin.clap", rich);
    auto json = a.to_json();

    HostScanCache b;
    REQUIRE(b.from_json(json));
    const auto& e = b.entries().at("/tmp/rich-plugin.clap");
    REQUIRE(e.info.category == "Analyzer");
    REQUIRE(e.info.features.size() == 3);
    REQUIRE(e.info.features[0] == "audio-effect");
    REQUIRE(e.info.features[1] == "analyzer");
    REQUIRE(e.info.features[2] == "utility");
    REQUIRE(e.info.description == "Real-time spectrum + phase analyzer.");
    REQUIRE(e.info.has_editor);
    REQUIRE(e.info.supports_sidechain);
    REQUIRE_FALSE(e.info.supports_midi_in);
    REQUIRE_FALSE(e.info.supports_midi_out);
}

TEST_CASE("from_json on pre-metadata blob leaves new fields at defaults",
          "[scan_cache][metadata][backcompat]") {
    // Schema-v1 cache blobs from before the richer-metadata fields
    // shipped must still deserialise cleanly with defaults — that's
    // the explicit scan_cache.hpp contract for older versions.
    std::string legacy = R"({
        "schema_version": 1,
        "entries": [{
            "path": "/tmp/legacy.vst3",
            "mtime": 0,
            "size": 0,
            "name": "Legacy",
            "manufacturer": "OldCorp",
            "version": "0.1",
            "plugin_path": "/tmp/legacy.vst3",
            "unique_id": "legacy-id",
            "format": "vst3",
            "is_instrument": false,
            "is_effect": true,
            "num_inputs": 2,
            "num_outputs": 2
        }]
    })";

    HostScanCache c;
    REQUIRE(c.from_json(legacy));
    REQUIRE(c.size() == 1);
    const auto& e = c.entries().at("/tmp/legacy.vst3");
    REQUIRE(e.info.name == "Legacy");
    REQUIRE(e.info.category.empty());
    REQUIRE(e.info.features.empty());
    REQUIRE(e.info.description.empty());
    REQUIRE_FALSE(e.info.has_editor);
    REQUIRE_FALSE(e.info.supports_sidechain);
    REQUIRE_FALSE(e.info.supports_midi_in);
    REQUIRE_FALSE(e.info.supports_midi_out);
}

TEST_CASE("from_json rejects wrong schema version", "[scan_cache]") {
    HostScanCache b;
    REQUIRE_FALSE(b.from_json(R"({"schema_version": 999, "entries": []})"));
    REQUIRE_FALSE(b.from_json("not json"));
}

TEST_CASE("ScanCache from_json accepts schema-only blob as an empty cache",
          "[scan_cache]") {
    HostScanCache cache;
    cache.put("/tmp/existing.vst3", sample_info());

    REQUIRE(cache.from_json(R"({"schema_version": 1})"));
    REQUIRE(cache.size() == 0);
    REQUIRE(cache.entries().empty());
}

TEST_CASE("ScanCache from_json skips malformed entries while loading valid entries",
          "[scan_cache]") {
    HostScanCache c;
    REQUIRE(c.from_json(R"({
        "schema_version": 1,
        "entries": [
            {
                "path": "/tmp/valid.vst3",
                "mtime": 0,
                "size": 0,
                "name": "Valid",
                "manufacturer": "Pulp",
                "version": "1.0",
                "plugin_path": "/tmp/valid.vst3",
                "unique_id": "valid-id",
                "format": "vst3",
                "is_instrument": false,
                "is_effect": true,
                "num_inputs": 2,
                "num_outputs": 2
            },
            {
                "path": "/tmp/bad-format.vst3",
                "format": "not-a-format"
            },
            "not an object"
        ]
    })"));

    REQUIRE(c.size() == 1);
    REQUIRE(c.entries().count("/tmp/valid.vst3") == 1);
}

TEST_CASE("ScanCache from_json keeps existing cache when blob is malformed",
          "[scan_cache]") {
    HostScanCache cache;
    cache.put("/tmp/existing.vst3", sample_info());

    REQUIRE_FALSE(cache.from_json("{"));
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.entries().count("/tmp/existing.vst3") == 1);
}

TEST_CASE("ScanCache loaded stale entry does not satisfy get", "[scan_cache]") {
    TempFile plugin;
    plugin.write("plugin payload");

    auto info = sample_info();
    info.path = plugin.path.string();

    HostScanCache fresh;
    fresh.put(plugin.path.string(), info);
    auto stale_json = fresh.to_json();

    std::this_thread::sleep_for(std::chrono::milliseconds(1100));
    plugin.write("plugin payload with a new stamp");

    HostScanCache loaded;
    REQUIRE(loaded.from_json(stale_json));
    REQUIRE(loaded.size() == 1);
    REQUIRE_FALSE(loaded.get(plugin.path.string()).has_value());
}

TEST_CASE("save_to / load_from round-trip via disk", "[scan_cache]") {
    TempFile f;
    // Use the parent dir for the cache file so save_to creates it.
    auto cache_path = (f.path.parent_path() / (f.path.filename().string() + ".json")).string();

    HostScanCache a;
    a.put("/tmp/does-not-exist.vst3", sample_info());
    REQUIRE(a.save_to(cache_path));

    HostScanCache b;
    REQUIRE(b.load_from(cache_path));
    REQUIRE(b.size() == 1);

    std::error_code ec;
    fs::remove(cache_path, ec);
}

TEST_CASE("ScanCache load_from missing file keeps existing cache", "[scan_cache]") {
    TempFile f;
    auto cache_path = (f.path.parent_path() / (f.path.filename().string() + ".missing")).string();

    HostScanCache cache;
    cache.put("/tmp/existing.vst3", sample_info());

    REQUIRE_FALSE(cache.load_from(cache_path));
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.entries().count("/tmp/existing.vst3") == 1);
}

TEST_CASE("ScanCache load_from empty or malformed file keeps existing cache",
          "[scan_cache]") {
    TempFile f;

    HostScanCache cache;
    cache.put("/tmp/existing.vst3", sample_info());

    f.write("");
    REQUIRE_FALSE(cache.load_from(f.path.string()));
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.entries().count("/tmp/existing.vst3") == 1);

    f.write("{");
    REQUIRE_FALSE(cache.load_from(f.path.string()));
    REQUIRE(cache.size() == 1);
    REQUIRE(cache.entries().count("/tmp/existing.vst3") == 1);
}
