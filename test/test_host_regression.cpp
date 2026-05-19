// Host regression coverage — issue #52.
//
// End-to-end host-subsystem regression tests focused on the surfaces that
// landed in #535 (VST3 FUID via moduleinfo.json, LV2 URI via manifest.ttl,
// null-slot pass-through, state round-trip) and the lifecycle phases
// called out in #52:
//
//   - Scan → load → process → unload (via the CLAP reference loader when
//     PulpGain.clap is built; skipped gracefully otherwise).
//   - Scan failure modes: moduleinfo.json missing → fallback, LV2 ttl
//     malformed → stem fallback, blacklist short-circuits re-scan.
//   - State save → restore → bit-identical buffer output.
//   - Hot-reload: swap a plugin slot mid-graph, verify stale-pointer-free.
//   - Automation: parameter events reach the plugin's process() call, and
//     host-side set_parameter is observable via get_parameter.
//
// Catch2 tag: [issue-52]. Tests that need an external plugin bundle skip
// with SUCCEED() when the bundle isn't available, so the suite passes on
// CI hosts regardless of configuration.
//
// Intentionally avoids wall-clock sleep_for in expectations — hot-reload
// uses an explicit atomic counter + future handshake instead (mirroring
// the #479 SpscQueue flake fix).

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scan_blacklist.hpp>
#include <pulp/host/scan_cache.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/signal_graph.hpp>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::host;
using Catch::Matchers::WithinAbs;

// ── Scratch-dir helper ────────────────────────────────────────────────────

namespace {

// RAII temp directory for synthetic plugin bundles + cache files.
struct ScratchDir {
    fs::path path;
    explicit ScratchDir(const char* stem) {
        auto counter = std::chrono::steady_clock::now().time_since_epoch().count();
        path = fs::temp_directory_path()
             / (std::string("pulp-host-regression-") + stem + "-"
                + std::to_string(counter));
        std::error_code ec;
        fs::remove_all(path, ec);
        fs::create_directories(path);
    }
    ~ScratchDir() { std::error_code ec; fs::remove_all(path, ec); }
    ScratchDir(const ScratchDir&) = delete;
    ScratchDir& operator=(const ScratchDir&) = delete;
};

void write_file(const fs::path& p, const std::string& content) {
    fs::create_directories(p.parent_path());
    std::ofstream f(p, std::ios::binary);
    REQUIRE(f.good());
    f << content;
}

// Build a minimal LV2 bundle skeleton with a specific manifest.ttl body.
fs::path make_lv2_bundle(const fs::path& parent,
                        const std::string& bundle_name,
                        const std::string& manifest_body) {
    auto bundle = parent / (bundle_name + ".lv2");
    fs::create_directories(bundle);
    write_file(bundle / "manifest.ttl", manifest_body);
    return bundle;
}

// Build a minimal VST3 bundle skeleton. `moduleinfo_body` may be empty to
// simulate a bundle that doesn't ship moduleinfo.json (fallback case).
fs::path make_vst3_bundle(const fs::path& parent,
                         const std::string& bundle_name,
                         const std::string& moduleinfo_body) {
    auto bundle = parent / (bundle_name + ".vst3");
    fs::create_directories(bundle / "Contents" / "Resources");
    if (!moduleinfo_body.empty()) {
        write_file(bundle / "Contents" / "Resources" / "moduleinfo.json",
                   moduleinfo_body);
    }
    return bundle;
}

#ifdef PULP_TEST_CLAP_PATH
fs::path copy_bundle_for_scan(const fs::path& source, const fs::path& parent) {
    const auto target = parent / source.filename();
    std::error_code ec;
    if (fs::is_directory(source, ec)) {
        fs::copy(source, target,
                 fs::copy_options::recursive | fs::copy_options::overwrite_existing,
                 ec);
    } else {
        fs::copy_file(source, target, fs::copy_options::overwrite_existing, ec);
    }
    REQUIRE_FALSE(ec);
    return target;
}
#endif

// Plain-text byte store used to drive bit-identical state round-trip
// assertions. MockStatefulPlugin exposes this as its state blob.
struct StateBlob {
    std::vector<uint8_t> bytes;
};

// Mock slot with a settable "gain" parameter, a stable state blob, and
// deterministic processing (out = in * gain). Used to exercise the
// save/restore/hot-reload/process lifecycle without a real plugin binary.
class MockStatefulPlugin final : public PluginSlot {
public:
    static constexpr uint32_t kGainParamId = 0x101;

    explicit MockStatefulPlugin(std::string name = "MockStateful") {
        info_.name = std::move(name);
        info_.num_inputs = 1;
        info_.num_outputs = 1;
        info_.format = PluginFormat::CLAP;
        info_.unique_id = "mock.stateful";
    }

    const PluginInfo& info() const override { return info_; }
    bool is_loaded() const override { return true; }
    bool prepare(double, int) override { prepared_ = true; return true; }
    void release() override { prepared_ = false; }

    void process(pulp::audio::BufferView<float>& out,
                 const pulp::audio::BufferView<const float>& in,
                 const pulp::midi::MidiBuffer&,
                 pulp::midi::MidiBuffer&,
                 const pulp::host::ParameterEventQueue& pe,
                 int n) override {
        // Apply any per-block param events. Use the most recent value.
        for (const auto& ev : pe) {
            if (ev.param_id == kGainParamId) {
                gain_.store(ev.value, std::memory_order_relaxed);
                received_events_.fetch_add(1, std::memory_order_relaxed);
            }
        }
        const float g = gain_.load(std::memory_order_relaxed);
        for (size_t c = 0; c < out.num_channels(); ++c) {
            float* d = out.channel_ptr(c);
            const float* s = c < in.num_channels() ? in.channel_ptr(c) : nullptr;
            for (int i = 0; i < n; ++i) d[i] = (s ? s[i] : 0.f) * g;
        }
        process_calls_.fetch_add(1, std::memory_order_relaxed);
    }

    std::vector<HostParamInfo> parameters() const override {
        HostParamInfo p;
        p.id = kGainParamId;
        p.name = "gain";
        p.min_value = 0.0f;
        p.max_value = 1.0f;
        p.default_value = 1.0f;
        p.flags.automatable = true;
        return { p };
    }
    float get_parameter(uint32_t id) const override {
        return id == kGainParamId ? gain_.load(std::memory_order_relaxed) : 0.f;
    }
    void set_parameter(uint32_t id, float v) override {
        if (id == kGainParamId) gain_.store(v, std::memory_order_relaxed);
    }
    void set_bypass(bool b) override { bypassed_ = b; }
    bool is_bypassed() const override { return bypassed_; }

    // State: 4-byte little-endian float (gain). Deliberately bit-stable so
    // we can assert byte-identical save/restore round-trips.
    std::vector<uint8_t> save_state() const override {
        std::vector<uint8_t> out(sizeof(float), 0);
        float g = gain_.load(std::memory_order_relaxed);
        std::memcpy(out.data(), &g, sizeof(float));
        return out;
    }
    bool restore_state(const std::vector<uint8_t>& data) override {
        if (data.size() != sizeof(float)) return false;
        float g = 0.f;
        std::memcpy(&g, data.data(), sizeof(float));
        gain_.store(g, std::memory_order_relaxed);
        return true;
    }
    bool has_editor() const override { return false; }
    void* create_editor_view() override { return nullptr; }
    void destroy_editor_view() override {}
    int latency_samples() const override { return 0; }
    int tail_samples() const override { return 0; }

    int process_call_count() const { return process_calls_.load(std::memory_order_relaxed); }
    int received_event_count() const { return received_events_.load(std::memory_order_relaxed); }
    bool prepared() const { return prepared_; }

private:
    PluginInfo info_;
    std::atomic<float> gain_{1.0f};
    std::atomic<int> process_calls_{0};
    std::atomic<int> received_events_{0};
    bool prepared_ = false;
    bool bypassed_ = false;
};

} // namespace

// ── Scanner regression: malformed manifest.ttl falls back to stem ──────────

TEST_CASE("PluginScanner LV2 bundle with malformed manifest.ttl falls back to stem",
          "[host][scanner][regression][issue-52]") {
    ScratchDir scratch("lv2-malformed");

    // A TTL body that looks non-empty but has no `a lv2:Plugin` stanza.
    // The regex must fail, and unique_id must fall back to the dir stem.
    const std::string body =
        "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
        "# intentionally malformed — no `a lv2:Plugin` anywhere\n"
        "<http://example.org/notaplugin> rdfs:comment \"orphan\" .\n";
    auto bundle = make_lv2_bundle(scratch.path, "Broken", body);

    ScanOptions opts;
    opts.scan_vst3 = false;
    opts.scan_clap = false;
    opts.scan_au = false;
    opts.scan_lv2 = true;
    opts.extra_paths.push_back(scratch.path.string());

    PluginScanner scanner;
    auto plugins = scanner.scan(opts);

    bool found = false;
    for (const auto& p : plugins) {
        if (p.path == bundle.string()) {
            found = true;
            REQUIRE(p.format == PluginFormat::LV2);
            // Malformed manifest → unique_id collapses to the stem. This
            // is the documented best-effort fallback contract.
            REQUIRE(p.unique_id == p.name);
            REQUIRE(p.name == "Broken");
        }
    }
    REQUIRE(found);
}

// ── Scanner regression: VST3 with missing moduleinfo.json falls back ──────

TEST_CASE("PluginScanner VST3 bundle with missing moduleinfo.json falls back to stem",
          "[host][scanner][regression][issue-52]") {
    ScratchDir scratch("vst3-no-moduleinfo");

    // Empty moduleinfo body = don't create the file at all.
    auto bundle = make_vst3_bundle(scratch.path, "NoModInfo", "");

    ScanOptions opts;
    opts.scan_clap = false;
    opts.scan_au = false;
    opts.scan_lv2 = false;
    opts.scan_vst3 = true;
    opts.extra_paths.push_back(scratch.path.string());

    PluginScanner scanner;
    auto plugins = scanner.scan(opts);

    bool found = false;
    for (const auto& p : plugins) {
        if (p.path != bundle.string()) continue;
        found = true;
        REQUIRE(p.format == PluginFormat::VST3);
        // No moduleinfo.json → stem fallback.
        REQUIRE(p.unique_id == p.name);
        REQUIRE(p.name == "NoModInfo");
    }
    REQUIRE(found);
}

// ── Scanner regression: VST3 moduleinfo.json with well-formed FUID ────────

TEST_CASE("PluginScanner VST3 bundle with well-formed moduleinfo.json yields FUID",
          "[host][scanner][regression][issue-52]") {
    ScratchDir scratch("vst3-fuid");

    // Minimal moduleinfo.json shape. The scanner walks Classes, picks the
    // first "Audio Module Class", and returns the normalized CID.
    const std::string body = R"({
  "Name": "FakePlugin",
  "Classes": [
    {
      "Category": "Audio Module Class",
      "CID": "0123456789ABCDEFFEDCBA9876543210",
      "Name": "FakePluginEffect"
    }
  ]
})";
    auto bundle = make_vst3_bundle(scratch.path, "FakeFUID", body);

    ScanOptions opts;
    opts.scan_clap = false;
    opts.scan_au = false;
    opts.scan_lv2 = false;
    opts.scan_vst3 = true;
    opts.extra_paths.push_back(scratch.path.string());

    PluginScanner scanner;
    auto plugins = scanner.scan(opts);

    bool found = false;
    for (const auto& p : plugins) {
        if (p.path != bundle.string()) continue;
        found = true;
        REQUIRE(p.format == PluginFormat::VST3);
        // 32-char lowercase hex FUID.
        REQUIRE(p.unique_id.size() == 32);
        REQUIRE(p.unique_id == "0123456789abcdeffedcba9876543210");
        // unique_id must NOT collapse to stem when the FUID is available.
        REQUIRE(p.unique_id != p.name);
    }
    REQUIRE(found);
}

TEST_CASE("PluginScanner VST3 moduleinfo raw CID bytes normalize to lowercase hex",
          "[host][scanner][regression][codecov][phase3]") {
    ScratchDir scratch("vst3-raw-cid");

    const std::string body = R"({
  "Classes": [
    {
      "Category": "Audio Module Class",
      "CID": "\u0001\u0002\u0003\u0004\u0005\u0006\u0007\u0008\u0009\u000a\u000b\u000c\u000d\u000e\u000f\u0010",
      "Name": "RawCidEffect"
    }
  ]
})";
    auto bundle = make_vst3_bundle(scratch.path, "RawCID", body);

    ScanOptions opts;
    opts.scan_clap = false;
    opts.scan_au = false;
    opts.scan_lv2 = false;
    opts.scan_vst3 = true;
    opts.only_extra_paths = true;
    opts.extra_paths.push_back(scratch.path.string());

    PluginScanner scanner;
    auto plugins = scanner.scan(opts);

    bool found = false;
    for (const auto& p : plugins) {
        if (p.path != bundle.string()) continue;
        found = true;
        REQUIRE(p.format == PluginFormat::VST3);
        REQUIRE(p.unique_id == "0102030405060708090a0b0c0d0e0f10");
        REQUIRE(p.unique_id != p.name);
    }
    REQUIRE(found);
}

// ── Scanner regression: malformed moduleinfo.json falls back cleanly ──────

TEST_CASE("PluginScanner VST3 bundle with malformed moduleinfo.json falls back to stem",
          "[host][scanner][regression][issue-52]") {
    ScratchDir scratch("vst3-bad-moduleinfo");

    // Invalid JSON — choc::json::parse throws, scanner must catch and
    // fall back silently (no crash on a user's plugin folder).
    auto bundle = make_vst3_bundle(scratch.path, "BadJson",
                                   "{ this is not json");

    ScanOptions opts;
    opts.scan_clap = false;
    opts.scan_au = false;
    opts.scan_lv2 = false;
    opts.scan_vst3 = true;
    opts.extra_paths.push_back(scratch.path.string());

    PluginScanner scanner;
    auto plugins = scanner.scan(opts);

    bool found = false;
    for (const auto& p : plugins) {
        if (p.path != bundle.string()) continue;
        found = true;
        REQUIRE(p.unique_id == p.name);
        REQUIRE(p.name == "BadJson");
    }
    REQUIRE(found);
}

// ── Scanner regression: blacklist short-circuits re-scan ──────────────────

TEST_CASE("PluginScanner honors blacklist and skips entry entirely",
          "[host][scanner][blacklist][regression][issue-52]") {
    ScratchDir scratch("scanner-blacklist");

    // A synthetic LV2 bundle and its "twin" — the scanner should find both
    // on a fresh scan, then only the non-blacklisted one on a re-scan.
    const std::string body =
        "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
        "<http://example.org/p1> a lv2:Plugin .\n";
    auto bad  = make_lv2_bundle(scratch.path, "Bad", body);
    auto good = make_lv2_bundle(scratch.path, "Good", body);

    ScanOptions opts;
    opts.scan_vst3 = false;
    opts.scan_clap = false;
    opts.scan_au = false;
    opts.scan_lv2 = true;
    opts.extra_paths.push_back(scratch.path.string());

    PluginScanner scanner;

    // First scan — both found.
    auto plugins = scanner.scan(opts);
    int fresh_count = 0;
    for (const auto& p : plugins) {
        if (p.path == bad.string() || p.path == good.string()) fresh_count++;
    }
    REQUIRE(fresh_count == 2);

    // Blacklist the bad bundle. ScanBlacklist uses the manifest.ttl's
    // (mtime, size) stamp, so we must blacklist an actual file. The
    // manifest.ttl within the bundle is fine — the scanner uses the
    // bundle directory path, not manifest.ttl. So we need to blacklist
    // the bundle's directory path. Blacklist matches on path string, and
    // re-stats that path to confirm the stamp still matches. For a
    // directory, stat returns the dir's own mtime/size.
    ScanBlacklist bl;
    bl.blacklist(bad.string(), "synthetic test blacklist");
    REQUIRE(bl.is_blacklisted(bad.string()));

    // Re-scan with blacklist — bad bundle skipped entirely.
    opts.blacklist = &bl;
    auto re = scanner.scan(opts);
    bool saw_bad = false, saw_good = false;
    for (const auto& p : re) {
        if (p.path == bad.string()) saw_bad = true;
        if (p.path == good.string()) saw_good = true;
    }
    REQUIRE_FALSE(saw_bad);
    REQUIRE(saw_good);
}

// ── Scan cache round-trip: save → load → hit ──────────────────────────────

TEST_CASE("HostScanCache round-trips a scanner result via disk",
          "[host][scanner][cache][regression][issue-52]") {
    ScratchDir scratch("scan-cache");

    // Write a real file to cache so the (mtime, size) stamp is stable.
    auto plugin_path = scratch.path / "fake.vst3";
    write_file(plugin_path, "not really a plugin, just a byte blob");

    PluginInfo info;
    info.name = "FakeCached";
    info.manufacturer = "PulpTest";
    info.version = "1.2.3";
    info.path = plugin_path.string();
    info.unique_id = "0123456789abcdeffedcba9876543210";
    info.format = PluginFormat::VST3;
    info.is_effect = true;
    info.num_inputs = 2;
    info.num_outputs = 2;
    info.category = "Fx|Dynamics";
    info.features = {"stereo", "dynamics"};
    info.description = "synthetic fixture";
    info.has_editor = true;
    info.supports_sidechain = true;

    auto cache_path = (scratch.path / "cache.json").string();
    {
        HostScanCache cache;
        cache.put(plugin_path.string(), info);
        REQUIRE(cache.size() == 1);
        REQUIRE(cache.save_to(cache_path));
    }

    // Fresh cache — load from disk and verify the entry round-trips.
    HostScanCache reloaded;
    REQUIRE(reloaded.load_from(cache_path));
    REQUIRE(reloaded.size() == 1);
    auto got = reloaded.get(plugin_path.string());
    REQUIRE(got.has_value());
    REQUIRE(got->unique_id == info.unique_id);
    REQUIRE(got->name == info.name);
    REQUIRE(got->manufacturer == info.manufacturer);
    REQUIRE(got->version == info.version);
    REQUIRE(got->category == info.category);
    REQUIRE(got->features == info.features);
    REQUIRE(got->description == info.description);
    REQUIRE(got->has_editor == info.has_editor);
    REQUIRE(got->supports_sidechain == info.supports_sidechain);
    REQUIRE(got->format == PluginFormat::VST3);
}

// ── Scan cache invalidates when file changes ──────────────────────────────

TEST_CASE("HostScanCache invalidates entry when file size changes",
          "[host][scanner][cache][regression][issue-52]") {
    ScratchDir scratch("scan-cache-invalidate");

    auto plugin_path = scratch.path / "fake.vst3";
    write_file(plugin_path, "v1 contents");

    PluginInfo info;
    info.name = "FakeV1";
    info.path = plugin_path.string();
    info.unique_id = "v1id";
    info.format = PluginFormat::VST3;

    HostScanCache cache;
    cache.put(plugin_path.string(), info);
    REQUIRE(cache.get(plugin_path.string()).has_value());

    // Change the file size — cache must invalidate.
    write_file(plugin_path, "v2 contents with noticeably different length");
    auto after = cache.get(plugin_path.string());
    REQUIRE_FALSE(after.has_value());
}

// ── Scan-options filters propagate through ────────────────────────────────

TEST_CASE("ScanOptions per-format disables gate scan_directory entries",
          "[host][scanner][regression][issue-52]") {
    ScratchDir scratch("scan-options");

    // One LV2 bundle + one VST3 bundle. Enabling only LV2 must hide VST3.
    const std::string ttl_body =
        "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
        "<http://example.org/lv2only> a lv2:Plugin .\n";
    auto lv2 = make_lv2_bundle(scratch.path, "OnlyLv2", ttl_body);
    auto vst = make_vst3_bundle(scratch.path, "OnlyVst3", "");

    ScanOptions opts;
    opts.scan_vst3 = false;
    opts.scan_clap = false;
    opts.scan_au = false;
    opts.scan_lv2 = true;
    opts.extra_paths.push_back(scratch.path.string());

    PluginScanner scanner;
    auto plugins = scanner.scan(opts);

    bool saw_lv2 = false, saw_vst = false;
    for (const auto& p : plugins) {
        if (p.path == lv2.string()) saw_lv2 = true;
        if (p.path == vst.string()) saw_vst = true;
    }
    REQUIRE(saw_lv2);
    REQUIRE_FALSE(saw_vst);
}

// ── Scanner progress callback is invoked ──────────────────────────────────

TEST_CASE("ScanOptions progress callback fires at least once per format lane",
          "[host][scanner][regression][issue-52]") {
    ScratchDir scratch("scan-progress");

    const std::string ttl_body =
        "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
        "<http://example.org/probe> a lv2:Plugin .\n";
    make_lv2_bundle(scratch.path, "Probe", ttl_body);

    std::atomic<int> calls{0};
    ScanOptions opts;
    opts.scan_vst3 = false;
    opts.scan_clap = false;
    opts.scan_au = false;
    opts.scan_lv2 = true;
    opts.extra_paths.push_back(scratch.path.string());
    opts.on_progress = [&](const std::string&, int, int) {
        calls.fetch_add(1, std::memory_order_relaxed);
    };

    PluginScanner scanner;
    (void)scanner.scan(opts);
    REQUIRE(calls.load() >= 1);
}

TEST_CASE("PluginScanner progress callback reports hermetic extra-path lanes",
          "[host][scanner][coverage][phase3]") {
    ScratchDir scratch("scan-progress-hermetic");

    const std::string ttl_body =
        "@prefix lv2: <http://lv2plug.in/ns/lv2core#> .\n"
        "<http://example.org/hermetic-lv2> a lv2:Plugin .\n";
    auto lv2 = make_lv2_bundle(scratch.path, "HermeticLv2", ttl_body);
    auto vst = make_vst3_bundle(scratch.path, "HermeticVst3", "");

    struct ProgressCall {
        std::string path;
        int scanned = -1;
        int total = -1;
    };
    std::vector<ProgressCall> calls;

    ScanOptions opts;
    opts.scan_vst3 = true;
    opts.scan_clap = false;
    opts.scan_au = true;
    opts.scan_lv2 = true;
    opts.only_extra_paths = true;
    opts.extra_paths.push_back(scratch.path.string());
    opts.on_progress = [&](const std::string& path, int scanned, int total) {
        calls.push_back({path, scanned, total});
    };

    PluginScanner scanner;
    auto plugins = scanner.scan(opts);

    REQUIRE(calls.size() == 2);
    REQUIRE(calls[0].path == scratch.path.string());
    REQUIRE(calls[0].scanned == 0);
    REQUIRE(calls[0].total == 0);
    REQUIRE(calls[1].path == scratch.path.string());
    REQUIRE(calls[1].scanned == 1);
    REQUIRE(calls[1].total == 0);

    bool saw_lv2 = false;
    bool saw_vst = false;
    for (const auto& p : plugins) {
        if (p.path == lv2.string()) saw_lv2 = true;
        if (p.path == vst.string()) saw_vst = true;
    }
    REQUIRE(saw_lv2);
    REQUIRE(saw_vst);
}

// ── Lifecycle: prepare → process → release on a mock slot ─────────────────

TEST_CASE("PluginSlot lifecycle: prepare toggles prepared, release flips it back",
          "[host][slot][regression][issue-52]") {
    MockStatefulPlugin plug;
    REQUIRE_FALSE(plug.prepared());
    REQUIRE(plug.prepare(48000.0, 64));
    REQUIRE(plug.prepared());
    plug.release();
    REQUIRE_FALSE(plug.prepared());
}

// ── State round-trip: save → restore → bit-identical output ───────────────

TEST_CASE("MockStatefulPlugin save_state -> restore_state yields bit-identical output",
          "[host][slot][state][regression][issue-52]") {
    // Plugin A: set gain to 0.375, run one block, capture output.
    auto a = std::make_unique<MockStatefulPlugin>();
    REQUIRE(a->prepare(48000.0, 32));
    a->set_parameter(MockStatefulPlugin::kGainParamId, 0.375f);
    auto state_a = a->save_state();

    const int n = 32;
    std::vector<float> in(n, 1.0f);
    std::vector<float> out_a(n, 0.f);
    const float* in_ptrs_a[1] = { in.data() };
    float*       out_ptrs_a[1] = { out_a.data() };
    pulp::audio::BufferView<const float> iv_a(in_ptrs_a, 1, n);
    pulp::audio::BufferView<float>       ov_a(out_ptrs_a, 1, n);
    pulp::midi::MidiBuffer mi_a, mo_a;
    pulp::host::ParameterEventQueue pe_a;
    a->process(ov_a, iv_a, mi_a, mo_a, pe_a, n);

    // Plugin B: fresh instance, restore from A's state, same input, same output.
    auto b = std::make_unique<MockStatefulPlugin>();
    REQUIRE(b->prepare(48000.0, 32));
    REQUIRE(b->restore_state(state_a));
    // Verify the restored param is observable via get_parameter.
    REQUIRE_THAT(b->get_parameter(MockStatefulPlugin::kGainParamId),
                 WithinAbs(0.375f, 1e-6f));

    std::vector<float> out_b(n, 0.f);
    const float* in_ptrs_b[1] = { in.data() };
    float*       out_ptrs_b[1] = { out_b.data() };
    pulp::audio::BufferView<const float> iv_b(in_ptrs_b, 1, n);
    pulp::audio::BufferView<float>       ov_b(out_ptrs_b, 1, n);
    pulp::midi::MidiBuffer mi_b, mo_b;
    pulp::host::ParameterEventQueue pe_b;
    b->process(ov_b, iv_b, mi_b, mo_b, pe_b, n);

    // Bit-identical: saved state fully determines post-restore behaviour.
    REQUIRE(std::memcmp(out_a.data(), out_b.data(),
                        sizeof(float) * static_cast<size_t>(n)) == 0);

    // And save_state on B now matches A's (byte-for-byte).
    auto state_b = b->save_state();
    REQUIRE(state_a == state_b);

    // Restore rejects malformed blobs.
    REQUIRE_FALSE(b->restore_state(std::vector<uint8_t>{0x00, 0x01, 0x02}));
}

// ── Graph lifecycle: scan-style identity round-trip into a live graph ─────

TEST_CASE("SignalGraph end-to-end: prepare -> process -> release with mock slot",
          "[host][graph][lifecycle][regression][issue-52]") {
    SignalGraph graph;
    auto in  = graph.add_input_node(1, "in");
    auto slot = std::make_unique<MockStatefulPlugin>();
    auto* slot_ptr = slot.get();
    auto plug = graph.add_plugin_node(std::move(slot), 1, 1, "mock");
    auto out = graph.add_output_node(1, "out");

    REQUIRE(graph.connect(in, 0, plug, 0));
    REQUIRE(graph.connect(plug, 0, out, 0));
    REQUIRE(graph.prepare(48000.0, 64));
    REQUIRE(slot_ptr->prepared());

    // Drive gain through the graph-level parameter API, then process a block.
    REQUIRE(graph.set_node_parameter(plug, MockStatefulPlugin::kGainParamId, 0.5f));
    REQUIRE_THAT(graph.get_node_parameter(plug, MockStatefulPlugin::kGainParamId),
                 WithinAbs(0.5f, 1e-6f));

    std::vector<float> in_buf(64, 1.0f), out_buf(64, 0.f);
    const float* in_ptrs[1]  = { in_buf.data() };
    float*       out_ptrs[1] = { out_buf.data() };
    pulp::audio::BufferView<const float> iv(in_ptrs, 1, 64);
    pulp::audio::BufferView<float>       ov(out_ptrs, 1, 64);
    graph.process(ov, iv, 64);

    for (int i = 0; i < 64; ++i) {
        REQUIRE_THAT(out_buf[i], WithinAbs(0.5f, 1e-6f));
    }
    REQUIRE(slot_ptr->process_call_count() == 1);

    graph.release();
}

// ── Automation: host → plugin param-event delivery in process() ───────────

TEST_CASE("ParameterEventQueue sorts automation events by sample offset",
          "[host][automation][issue-493]") {
    pulp::host::ParameterEventQueue queue;
    queue.push({MockStatefulPlugin::kGainParamId, 24, 0.75f});
    queue.push({MockStatefulPlugin::kGainParamId, 0, 0.25f});
    queue.push({0x202, 12, 0.50f});

    REQUIRE_FALSE(queue.empty());
    REQUIRE(queue.size() == 3);

    queue.sort();

    auto it = queue.begin();
    REQUIRE(it->sample_offset == 0);
    REQUIRE(it->param_id == MockStatefulPlugin::kGainParamId);
    REQUIRE_THAT(it->value, WithinAbs(0.25f, 1e-6f));

    ++it;
    REQUIRE(it->sample_offset == 12);
    REQUIRE(it->param_id == 0x202);
    REQUIRE_THAT(it->value, WithinAbs(0.50f, 1e-6f));

    ++it;
    REQUIRE(it->sample_offset == 24);
    REQUIRE(it->param_id == MockStatefulPlugin::kGainParamId);
    REQUIRE_THAT(it->value, WithinAbs(0.75f, 1e-6f));
}

TEST_CASE("ParameterEventQueue clear makes the queue reusable",
          "[host][automation][issue-493]") {
    pulp::host::ParameterEventQueue queue;
    queue.push({MockStatefulPlugin::kGainParamId, 8, 0.5f});
    REQUIRE(queue.size() == 1);

    queue.clear();
    REQUIRE(queue.empty());
    REQUIRE(queue.events().empty());

    queue.push({0x303, 4, 1.0f});
    REQUIRE(queue.size() == 1);
    REQUIRE(queue.begin()->param_id == 0x303);
    REQUIRE(queue.begin()->sample_offset == 4);
    REQUIRE_THAT(queue.begin()->value, WithinAbs(1.0f, 1e-6f));
}

TEST_CASE("ParameterEventQueue has fixed capacity and drops overflow without allocation",
          "[host][automation][regression]") {
    pulp::host::ParameterEventQueue queue;
    REQUIRE(queue.capacity() == pulp::host::ParameterEventQueue::kCapacity);

    for (std::size_t i = 0; i < queue.capacity(); ++i) {
        REQUIRE(queue.push({MockStatefulPlugin::kGainParamId,
                            static_cast<int32_t>(i),
                            static_cast<float>(i)}));
    }

    REQUIRE_FALSE(queue.push({MockStatefulPlugin::kGainParamId, 9999, 1.0f}));
    REQUIRE(queue.size() == queue.capacity());
    REQUIRE(queue.events().back().sample_offset ==
            static_cast<int32_t>(queue.capacity() - 1));
}

TEST_CASE("ParameterEventQueue accepts rvalue pushes and exposes const iteration",
          "[host][automation][coverage][phase3]") {
    pulp::host::ParameterEventQueue queue;
    queue.push(pulp::host::ParameterEvent{0x10, 5, 0.5f});
    queue.push(pulp::host::ParameterEvent{0x11, 5, 0.6f});
    queue.push(pulp::host::ParameterEvent{0x12, 1, 0.2f});

    queue.sort();

    const auto& const_queue = queue;
    auto it = const_queue.begin();
    REQUIRE(it != const_queue.end());
    REQUIRE(it->param_id == 0x12);
    REQUIRE(it->sample_offset == 1);

    ++it;
    REQUIRE(it->sample_offset == 5);
    REQUIRE(it->param_id == 0x10);

    ++it;
    REQUIRE(it->sample_offset == 5);
    REQUIRE(it->param_id == 0x11);
}

TEST_CASE("SignalGraph delivers set_parameter via ParameterEventQueue",
          "[host][graph][automation][regression][issue-52]") {
    SignalGraph graph;
    auto in = graph.add_input_node(1, "in");
    auto slot = std::make_unique<MockStatefulPlugin>();
    auto* slot_ptr = slot.get();
    auto plug = graph.add_plugin_node(std::move(slot), 1, 1, "mock");
    auto out = graph.add_output_node(1, "out");

    // Automation: the input node's port 0 audio sample drives the gain
    // parameter. Two control points per block — sample 0 and sample N-1.
    REQUIRE(graph.connect(in, 0, plug, 0));
    REQUIRE(graph.connect(plug, 0, out, 0));
    REQUIRE(graph.connect_automation(
        in, 0, plug, MockStatefulPlugin::kGainParamId, 0.0f, 1.0f));
    REQUIRE(graph.prepare(48000.0, 32));

    // Input rises from 0.25 to 0.75 across the block — the mock slot
    // records every event it receives.
    std::vector<float> in_buf(32, 0.f);
    in_buf[0]  = 0.25f;
    in_buf[31] = 0.75f;
    std::vector<float> out_buf(32, 0.f);
    const float* in_ptrs[1]  = { in_buf.data() };
    float*       out_ptrs[1] = { out_buf.data() };
    pulp::audio::BufferView<const float> iv(in_ptrs, 1, 32);
    pulp::audio::BufferView<float>       ov(out_ptrs, 1, 32);
    graph.process(ov, iv, 32);

    // Two events per block (sample 0 + sample N-1). Automation delivery
    // proves host → plugin routing works end-to-end.
    REQUIRE(slot_ptr->received_event_count() == 2);
    // Final gain matches the last event value (0.75).
    REQUIRE_THAT(slot_ptr->get_parameter(MockStatefulPlugin::kGainParamId),
                 WithinAbs(0.75f, 1e-6f));
}

// ── Hot-reload: swap a plugin slot mid-graph via remove → add ─────────────
//
// The task calls out "replace a slot mid-graph → no race, no stale pointers"
// as a regression surface. SignalGraph's documented reload protocol is
// "mutate, then prepare()"; the audio thread sees silence between. We
// verify the sequence completes without crash and emits the expected
// outputs from both the old and new slot.

TEST_CASE("SignalGraph hot-reload swaps a plugin slot without stale pointers",
          "[host][graph][hot-reload][regression][issue-52]") {
    SignalGraph graph;
    auto in  = graph.add_input_node(1, "in");
    auto slot1 = std::make_unique<MockStatefulPlugin>("first");
    auto* slot1_ptr = slot1.get();
    auto plug = graph.add_plugin_node(std::move(slot1), 1, 1, "first");
    auto out = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(in, 0, plug, 0));
    REQUIRE(graph.connect(plug, 0, out, 0));
    REQUIRE(graph.prepare(48000.0, 16));
    slot1_ptr->set_parameter(MockStatefulPlugin::kGainParamId, 0.25f);

    // Block 1 — first slot runs.
    std::vector<float> in_buf(16, 1.0f), out_buf(16, 0.f);
    const float* in_ptrs[1]  = { in_buf.data() };
    float*       out_ptrs[1] = { out_buf.data() };
    pulp::audio::BufferView<const float> iv(in_ptrs, 1, 16);
    pulp::audio::BufferView<float>       ov(out_ptrs, 1, 16);
    graph.process(ov, iv, 16);
    for (int i = 0; i < 16; ++i) {
        REQUIRE_THAT(out_buf[i], WithinAbs(0.25f, 1e-6f));
    }
    REQUIRE(slot1_ptr->process_call_count() == 1);

    // Save first slot's state so we can assert identity transfer.
    auto state = slot1_ptr->save_state();

    // Hot-reload: remove the old plugin node, add a new one wired into
    // the same topology, re-prepare.
    REQUIRE(graph.remove_node(plug));
    auto slot2 = std::make_unique<MockStatefulPlugin>("second");
    auto* slot2_ptr = slot2.get();
    auto plug2 = graph.add_plugin_node(std::move(slot2), 1, 1, "second");
    REQUIRE(graph.connect(in, 0, plug2, 0));
    REQUIRE(graph.connect(plug2, 0, out, 0));

    // Between mutation and prepare, process() must return silence — not
    // crash via a stale plugin pointer. Drive a block through the
    // un-prepared snapshot and assert zeros.
    std::fill(out_buf.begin(), out_buf.end(), 99.0f);
    graph.process(ov, iv, 16);
    for (int i = 0; i < 16; ++i) REQUIRE(out_buf[i] == 0.0f);

    // Restore the saved state into the new slot and re-prepare.
    REQUIRE(slot2_ptr->restore_state(state));
    REQUIRE(graph.prepare(48000.0, 16));

    // Block 2 — second slot runs with the transferred state.
    std::fill(out_buf.begin(), out_buf.end(), 99.0f);
    graph.process(ov, iv, 16);
    for (int i = 0; i < 16; ++i) {
        REQUIRE_THAT(out_buf[i], WithinAbs(0.25f, 1e-6f));
    }
    REQUIRE(slot2_ptr->process_call_count() == 1);

    graph.release();
}

// ── Concurrent hot-reload: audio thread + UI thread race with explicit
//    handshake (no wall-clock sleeps in expectations).

// Race-condition regression — see pulp#669 for the open investigation.
//
// Status (2026-04-22): the test is FLAKY on Namespace runners (Linux + Windows)
// and CANNOT be reproduced locally on macOS even with all-core CPU saturation.
// The snapshot-publish path in `SignalGraph::prepare` uses acquire/release
// atomics on `live_`, and `compile_()` builds a fresh CompiledGraph with its
// own per-node runtime buffers and shared_ptr<PluginSlot> entries, so the
// classical "audio thread sees half-built cg" footgun should not apply.
//
// Working theories (to verify with the CTest stress target gated by
// PULP_STRESS_FLAKY_TESTS=1 added in test/CMakeLists.txt — repeats this case
// 100x per ctest run):
//
//   1. Namespace's shared-tenant ARM hardware exposes a memory-ordering
//      window the test's invariant `out ∈ {0, 1.0}` is sensitive to but
//      Apple Silicon doesn't observably hit. ARMv8 is weaker than x86 on
//      acquire/release semantics across cores; a missed release store on
//      a transitive plugin field could leak through.
//   2. `MockStatefulPlugin::process` reads `gain_` with `memory_order_relaxed`.
//      Combined with the slot's destructor running on the main thread while
//      the audio thread holds an OLD cg's shared_ptr<PluginSlot>, a torn
//      relaxed read after destructor begins would produce indeterminate
//      gain. shared_ptr's atomic refcount blocks destruction during the
//      audio thread's call, so this should be impossible — but the test
//      stresses it hard enough that any subtle violation surfaces.
//   3. Latent compiler / TSan reordering of the `nodes_` mutation vs the
//      `invalidate_live_()` release barrier on the main thread. We invalidate
//      AFTER each mutator finishes touching nodes_/connections_, but the
//      reordering window between the two is wider than it needs to be.
//
// First diagnostic step before fix: run `PULP_STRESS_FLAKY_TESTS=1 ctest -R
// pulp-test-signal-graph-hot-reload-stress` on a Namespace runner with TSan
// enabled and inspect whatever first race-report fires.
TEST_CASE("SignalGraph hot-reload mid-audio is race-free via snapshot publish",
          "[host][graph][hot-reload][race][regression][issue-669]") {
    SignalGraph graph;
    auto in  = graph.add_input_node(1, "in");
    auto slot = std::make_unique<MockStatefulPlugin>();
    auto plug = graph.add_plugin_node(std::move(slot), 1, 1, "a");
    auto out = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(in, 0, plug, 0));
    REQUIRE(graph.connect(plug, 0, out, 0));
    REQUIRE(graph.prepare(48000.0, 16));

    std::atomic<bool> stop{false};
    std::atomic<int>  blocks{0};
    std::atomic<int>  bad_samples{0};

    std::promise<void> audio_started;
    auto started = audio_started.get_future();
    std::promise<void> first_block_processed;
    auto first_block = first_block_processed.get_future();

    std::thread audio([&] {
        std::vector<float> in_buf(16, 1.0f), out_buf(16, 0.f);
        const float* in_ptrs[1]  = { in_buf.data() };
        float*       out_ptrs[1] = { out_buf.data() };
        pulp::audio::BufferView<const float> iv(in_ptrs, 1, 16);
        pulp::audio::BufferView<float>       ov(out_ptrs, 1, 16);
        audio_started.set_value();
        bool first_block_signalled = false;
        while (!stop.load(std::memory_order_relaxed)) {
            graph.process(ov, iv, 16);
            // Valid outputs: 0 (invalidated snapshot) OR 1.0 (unity gain).
            for (int i = 0; i < 16; ++i) {
                const float v = out_buf[i];
                if (!(v == 0.0f || v == 1.0f)) {
                    bad_samples.fetch_add(1, std::memory_order_relaxed);
                }
            }
            blocks.fetch_add(1, std::memory_order_relaxed);
            if (!first_block_signalled) {
                first_block_processed.set_value();
                first_block_signalled = true;
            }
        }
    });

    // Deterministic handshake: wait for the audio thread to start before
    // we begin mutating, then wait for one block to complete so the test
    // cannot pass or fail without exercising graph.process at least once.
    started.wait();
    first_block.wait();

    // Hammer the graph with remove → add → prepare cycles. Each iteration
    // drops the live snapshot and republishes a fresh one; the audio
    // thread either sees silence or the new slot's unity-gain output.
    constexpr int kCycles = 50;
    NodeId current = plug;
    for (int i = 0; i < kCycles; ++i) {
        REQUIRE(graph.remove_node(current));
        auto next_slot = std::make_unique<MockStatefulPlugin>();
        current = graph.add_plugin_node(std::move(next_slot), 1, 1, "hot");
        REQUIRE(graph.connect(in, 0, current, 0));
        REQUIRE(graph.connect(current, 0, out, 0));
        REQUIRE(graph.prepare(48000.0, 16));
    }

    // Let the audio thread observe the final snapshot. We don't assert
    // count — just that the race-correctness invariant held.
    stop.store(true, std::memory_order_relaxed);
    audio.join();

    REQUIRE(blocks.load() > 0);
    REQUIRE(bad_samples.load() == 0);
}

// ── Scanner → loader integration on a real CLAP plugin, when built ────────

#ifdef PULP_TEST_CLAP_PATH
TEST_CASE("Scanner -> load -> process -> unload round-trip on real CLAP plugin",
          "[host][scanner][clap][integration][regression][issue-52]") {
    const fs::path clap_path = PULP_TEST_CLAP_PATH;
    if (!fs::exists(clap_path)) {
        SUCCEED("PulpGain.clap not built — skipping integration");
        return;
    }

    // Point the scanner at a scratch directory containing only PulpGain so
    // it discovers the plugin via the normal path (NOT default_paths, which
    // would collide with system-installed bundles on the dev's machine).
    // Codex 2026-04-21 review on #545: `extra_paths` alone does not
    // suppress the default scan roots — the scanner still enumerated
    // every user/system CLAP, which can execute unrelated plugin
    // `clap_entry` code during CI. `only_extra_paths = true` restricts
    // the scan to the scratch directory. Keep that directory single-plugin:
    // the build output directory may contain other CLAP examples whose
    // descriptor probes should not affect this load/process/unload test.
    ScratchDir scratch("clap-isolated");
    const fs::path isolated_clap_path = copy_bundle_for_scan(clap_path, scratch.path);
    auto parent = isolated_clap_path.parent_path().string();
    ScanOptions opts;
    opts.scan_vst3 = false;
    opts.scan_au = false;
    opts.scan_lv2 = false;
    opts.scan_clap = true;
    opts.only_extra_paths = true;
    opts.extra_paths.push_back(parent);

    PluginScanner scanner;
    auto plugins = scanner.scan(opts);
    REQUIRE(plugins.size() == 1);
    const PluginInfo* found = nullptr;
    for (const auto& p : plugins) {
        if (p.path == isolated_clap_path.string()) { found = &p; break; }
    }
    REQUIRE(found != nullptr);
    REQUIRE(found->format == PluginFormat::CLAP);
    // CLAP bundles expose their descriptor id as unique_id.
    REQUIRE_FALSE(found->unique_id.empty());

    // Load via the normal PluginSlot::load() path.
    auto slot = PluginSlot::load(*found);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->is_loaded());
    REQUIRE(slot->prepare(48000.0, 256));

    std::vector<float> in_l(256, 0.5f), in_r(256, 0.5f);
    std::vector<float> out_l(256, 0.f), out_r(256, 0.f);
    const float* in_ptrs[2]  = { in_l.data(), in_r.data() };
    float*       out_ptrs[2] = { out_l.data(), out_r.data() };
    pulp::audio::BufferView<const float> iv(in_ptrs, 2, 256);
    pulp::audio::BufferView<float>       ov(out_ptrs, 2, 256);
    pulp::midi::MidiBuffer mi, mo;
    pulp::host::ParameterEventQueue pe;
    slot->process(ov, iv, mi, mo, pe, 256);

    float energy = 0.f;
    for (int i = 0; i < 256; ++i) energy += std::abs(out_l[i]) + std::abs(out_r[i]);
    REQUIRE(energy > 0.0f);

    // State round-trip on a real plugin — save, mutate, restore, observe.
    auto state = slot->save_state();
    // Not every CLAP plugin persists state; tolerate empty blobs (skip
    // the restore assertion rather than failing the integration).
    if (!state.empty()) {
        REQUIRE(slot->restore_state(state));
    }

    slot->release();
}
#endif

// Codex 2026-04-21 review on #545: sanity-check that `only_extra_paths`
// actually suppresses the default scan roots. Uses an empty scratch
// directory so the scanner has nothing legitimate to find — under the
// fix, the plugin count is zero; under the old behaviour it would
// happily enumerate every user/system CLAP on the dev's machine.
TEST_CASE("PluginScanner::scan honors only_extra_paths",
          "[host][scanner][issue-545][codex-545]") {
    using pulp::host::PluginScanner;
    using pulp::host::ScanOptions;

    auto scratch = std::filesystem::temp_directory_path() /
        ("pulp-scanner-hermetic-" +
         std::to_string(static_cast<uint64_t>(std::chrono::steady_clock::now()
             .time_since_epoch().count())));
    std::filesystem::create_directories(scratch);

    ScanOptions opts;
    opts.scan_vst3 = true;
    opts.scan_au = true;
    opts.scan_clap = true;
    opts.scan_lv2 = false;
    opts.only_extra_paths = true;
    opts.extra_paths.push_back(scratch.string());

    PluginScanner scanner;
    auto plugins = scanner.scan(opts);

    std::filesystem::remove_all(scratch);

    // Empty scratch dir + only_extra_paths → zero results. If the old
    // behaviour leaked back in, this assertion fires because the dev's
    // installed plugins would be picked up.
    REQUIRE(plugins.empty());
}
