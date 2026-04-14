// Unit tests for HostScanCache (workstream 03 slice 3.1).

#include <catch2/catch_test_macros.hpp>
#include <pulp/host/scan_cache.hpp>

#include <chrono>
#include <filesystem>
#include <fstream>
#include <thread>

using namespace pulp::host;
namespace fs = std::filesystem;

namespace {

struct TempFile {
    fs::path path;
    TempFile() {
        auto stem = "pulp-scan-cache-" + std::to_string(
            std::chrono::steady_clock::now().time_since_epoch().count());
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

TEST_CASE("from_json rejects wrong schema version", "[scan_cache]") {
    HostScanCache b;
    REQUIRE_FALSE(b.from_json(R"({"schema_version": 999, "entries": []})"));
    REQUIRE_FALSE(b.from_json("not json"));
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
