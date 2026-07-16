#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/signal_graph.hpp>
// The CLAP fixture is a Pulp-built plugin, so its editor obeys Pulp's own
// editor-environment gate; the host-side expectation has to read the same flag.
#include <pulp/format/detail/editor_environment.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>
#include "harness/rt_allocation_probe.hpp"
#include <array>
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <future>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using namespace pulp::host;
using Catch::Matchers::WithinAbs;

// ── Scanner tests ───────────────────────────────────────────────────────

TEST_CASE("PluginScanner default paths", "[host][scanner]") {
    SECTION("VST3 paths are non-empty on all platforms") {
        auto paths = PluginScanner::default_paths(PluginFormat::VST3);
#ifdef __APPLE__
        REQUIRE(paths.size() >= 2); // user + system
#elif defined(_WIN32)
        REQUIRE(paths.size() >= 1);
#elif defined(__linux__)
        REQUIRE(paths.size() >= 2);
#endif
    }

    SECTION("CLAP paths") {
        auto paths = PluginScanner::default_paths(PluginFormat::CLAP);
        REQUIRE_FALSE(paths.empty());
    }
}

TEST_CASE("HostParamInfo defaults to control rate and can mark audio-rate params",
          "[host][params][rate]") {
    HostParamInfo info;
    REQUIRE(info.rate == ParamRate::ControlRate);

    info.rate = ParamRate::AudioRate;
    REQUIRE(info.rate == pulp::state::ParamRate::AudioRate);
}

TEST_CASE("PluginScanner bundle detection", "[host][scanner]") {
    REQUIRE(PluginScanner::is_plugin_bundle("MyPlugin.vst3", PluginFormat::VST3));
    REQUIRE(PluginScanner::is_plugin_bundle("MyPlugin.clap", PluginFormat::CLAP));
    REQUIRE(PluginScanner::is_plugin_bundle("MyPlugin.component", PluginFormat::AudioUnit));
    REQUIRE(PluginScanner::is_plugin_bundle("MyPlugin.lv2", PluginFormat::LV2));
    REQUIRE_FALSE(PluginScanner::is_plugin_bundle("MyPlugin.dll", PluginFormat::VST3));
}

TEST_CASE("AU / AUv3 post-scan narrowing keeps exact flavor",
          "[host][scanner][issue-500]") {
    // Regression for issue #500: `pulp scan` advertised `--format au`
    // and `--format auv3` as mutually-exclusive filters, but both values
    // set the same ScanOptions::scan_au flag, and PluginScanner::
    // scan_audio_units() returns mixed AU + AUv3 entries. The CLI now runs
    // a post-scan narrowing step — this test models that step on a
    // synthetic mixed slice so the narrowing logic is covered independently
    // of any installed AudioComponents.
    std::vector<PluginInfo> mixed;
    PluginInfo v2;  v2.format = PluginFormat::AudioUnit;    v2.name = "EffectV2";
    PluginInfo v3;  v3.format = PluginFormat::AudioUnitV3;  v3.name = "EffectV3";
    PluginInfo v2b; v2b.format = PluginFormat::AudioUnit;   v2b.name = "SynthV2";
    mixed = {v2, v3, v2b};

    auto narrow = [&](PluginFormat wanted) {
        auto out = mixed;
        out.erase(std::remove_if(out.begin(), out.end(),
            [&](const PluginInfo& info) { return info.format != wanted; }),
            out.end());
        return out;
    };

    SECTION("asking for AU v2 drops AUv3 entries") {
        auto v2_only = narrow(PluginFormat::AudioUnit);
        REQUIRE(v2_only.size() == 2);
        for (const auto& info : v2_only) {
            REQUIRE(info.format == PluginFormat::AudioUnit);
        }
    }

    SECTION("asking for AUv3 drops AU v2 entries") {
        auto v3_only = narrow(PluginFormat::AudioUnitV3);
        REQUIRE(v3_only.size() == 1);
        REQUIRE(v3_only[0].format == PluginFormat::AudioUnitV3);
        REQUIRE(v3_only[0].name == "EffectV3");
    }
}

TEST_CASE("PluginScanner scan runs without crash", "[host][scanner]") {
    PluginScanner scanner;
    ScanOptions opts;
    opts.scan_lv2 = false; // LV2 not typical in test environments
    auto plugins = scanner.scan(opts);
    // May find real plugins on the system — just verify no crash
    REQUIRE(plugins.size() >= 0);
}

TEST_CASE("cap_clap_plugin_count clamps an untrusted bundle count (issue-2703)",
          "[host][scanner][issue-2703]") {
    using pulp::host::cap_clap_plugin_count;
    // Sane counts pass through unchanged.
    REQUIRE(cap_clap_plugin_count(0) == 0);
    REQUIRE(cap_clap_plugin_count(1) == 1);
    REQUIRE(cap_clap_plugin_count(64) == 64);
    REQUIRE(cap_clap_plugin_count(1024) == 1024);
    // An absurd count from a malformed factory is clamped before it can drive
    // an allocation that would throw and abort the whole scan.
    REQUIRE(cap_clap_plugin_count(1025) == 1024);
    REQUIRE(cap_clap_plugin_count(1000000) == 1024);
    REQUIRE(cap_clap_plugin_count(0xFFFFFFFFu) == 1024);
}

// Regression test for #1862: scanner_clap.cpp hit a `dlerror()`
// double-call SEGV under ASan. The defensive #812 fallback path in
// `scan_clap_bundle_descriptors` logs the dlerror() string when dlopen
// fails, but the original code called `dlerror()` TWICE in a
// `cond ? dlerror() : "..."` ternary. POSIX
// dlerror() clears its internal buffer after each call, so the second
// invocation returns nullptr — which `std::format`'s
// `string_view(char const*)` ctor then passes to `strlen(nullptr)`,
// crashing under ASan with a SEGV on libsystem_platform's
// _platform_strlen.
//
// Pin: scan a malformed `.clap` bundle and assert (a) it doesn't crash
// and (b) the filename-fallback path produces a single PluginInfo so
// users see SOMETHING in the catalog instead of a silent drop.
TEST_CASE("scan_clap_bundle_descriptors survives malformed bundle (dlopen-fail path)",
          "[host][scanner][issue-1862]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "pulp_clap_dlopen_fail_test.clap";
    std::error_code ec;
    fs::remove_all(tmp, ec);

    // A `.clap` bundle whose binary is a plain text file rather than a
    // valid dylib. macOS dlopen / Linux dlopen will both fail; the
    // dlerror() will be non-empty on the first call and (originally)
    // null on the second. Our fix caches the first call in a local.
    //
    // Bundle layout matches what `resolve_clap_binary` expects on each
    // platform; on macOS, that's `<bundle>.clap/Contents/MacOS/<name>`.
    // On Linux / Windows the resolver looks for a flat binary path —
    // either way, `dlopen()` will fail on garbage contents.
#if defined(__APPLE__)
    auto inner = tmp / "Contents" / "MacOS";
    fs::create_directories(inner, ec);
    REQUIRE(!ec);
    std::ofstream(inner / "pulp_clap_dlopen_fail_test")
        << "not a real dylib — should fail dlopen()\n";
#else
    fs::create_directories(tmp, ec);
    REQUIRE(!ec);
    std::ofstream(tmp / "pulp_clap_dlopen_fail_test.so")
        << "not a real shared object\n";
#endif

    // Drive through PluginScanner::scan() with hermetic extra_paths so
    // the malformed bundle is discovered through the normal scan path
    // and we exercise the dlopen-fail branch deterministically without
    // requiring a system CLAP folder. `only_extra_paths = true` keeps
    // the test from walking the dev's installed CLAP collection (#545).
    PluginScanner scanner;
    ScanOptions opts;
    opts.scan_vst3 = false;
    opts.scan_au = false;
    opts.scan_lv2 = false;
    opts.scan_clap = true;
    opts.only_extra_paths = true;
    opts.extra_paths = { tmp.parent_path().string() };
    auto results = scanner.scan(opts);

    // The fix returns a filename-fallback entry instead of crashing.
    // Find the entry for our specific test bundle (other CLAP bundles
    // may exist in tmp on shared CI runners).
    bool found = false;
    for (const auto& info : results) {
        if (info.path == tmp.string()) {
            REQUIRE(info.format == PluginFormat::CLAP);
            // make_filename_fallback derives a non-empty name from
            // the bundle stem.
            REQUIRE(!info.name.empty());
            found = true;
            break;
        }
    }
    REQUIRE(found);

    fs::remove_all(tmp, ec);
}

// Issue #491: scan_lv2_bundle must set unique_id to the plugin URI
// parsed from manifest.ttl, not the filesystem stem. This keeps
// graph_serializer rehydration stable across sessions even when two
// LV2 bundles share a directory name.
TEST_CASE("PluginScanner LV2 bundle uses URI from manifest.ttl as unique_id",
          "[host][scanner][issue-491]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "pulp_lv2_scan_test.lv2";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    // Minimal manifest.ttl in the canonical LV2 shape.
    const char* manifest = R"(
@prefix lv2:  <http://lv2plug.in/ns/lv2core#> .
@prefix rdfs: <http://www.w3.org/2000/01/rdf-schema#> .

<http://example.com/plugins/PulpTestCompressor> a lv2:Plugin ;
    lv2:binary <plugin.so> ;
    rdfs:seeAlso <plugin.ttl> .
)";
    {
        std::ofstream f(tmp / "manifest.ttl");
        REQUIRE(f.good());
        f << manifest;
    }

    ScanOptions opts;
    opts.scan_vst3 = false;
    opts.scan_clap = false;
    opts.scan_au = false;
    opts.scan_lv2 = true;
    opts.extra_paths.push_back(tmp.parent_path().string());

    PluginScanner scanner;
    auto all = scanner.scan(opts);
    bool found = false;
    for (const auto& p : all) {
        if (p.path == tmp.string()) {
            found = true;
            // The stable identifier is the URI from manifest.ttl.
            REQUIRE(p.unique_id == "http://example.com/plugins/PulpTestCompressor");
            // unique_id must not collapse to the filesystem stem.
            REQUIRE(p.unique_id != p.name);
            REQUIRE(p.unique_id.find("http") != std::string::npos);
        }
    }
    REQUIRE(found);

    fs::remove_all(tmp, ec);
}

TEST_CASE("PluginScanner LV2 bundle falls back to stem when manifest.ttl missing",
          "[host][scanner][issue-491]") {
    namespace fs = std::filesystem;
    auto tmp = fs::temp_directory_path() / "pulp_lv2_nomanifest_test.lv2";
    std::error_code ec;
    fs::remove_all(tmp, ec);
    fs::create_directories(tmp);

    ScanOptions opts;
    opts.scan_vst3 = false;
    opts.scan_clap = false;
    opts.scan_au = false;
    opts.scan_lv2 = true;
    opts.extra_paths.push_back(tmp.parent_path().string());

    PluginScanner scanner;
    auto all = scanner.scan(opts);
    for (const auto& p : all) {
        if (p.path == tmp.string()) {
            // Graceful fallback: use the display name when no manifest URI exists.
            REQUIRE(p.unique_id == p.name);
        }
    }

    fs::remove_all(tmp, ec);
}

// Issue #491: scan_vst3_bundle must set unique_id to the VST3 FUID
// (PClassInfo::cid serialized as 32-char lowercase hex) when the SDK
// is available, not the display name. This walks any VST3 plugins
// installed on the CI host; the check is a structural assertion —
// unique_id must be a 32-char lowercase hex string or the stem
// fallback. CI hosts without a VST3 install skip gracefully.
TEST_CASE("PluginScanner VST3 bundle uses FUID as unique_id when SDK available",
          "[host][scanner][issue-491]") {
    PluginScanner scanner;
    ScanOptions opts;
    opts.scan_clap = false;
    opts.scan_au = false;
    opts.scan_lv2 = false;
    opts.scan_vst3 = true;
    auto plugins = scanner.scan(opts);

    int fuid_shaped = 0;
    int total_vst3 = 0;
    for (const auto& p : plugins) {
        if (p.format != PluginFormat::VST3) continue;
        total_vst3++;
        // unique_id must either:
        //  (a) equal the filesystem stem — fallback path when the
        //      bundle couldn't be opened or VST3 SDK wasn't linked,
        //      matching the best-effort fallback contract; or
        //  (b) be a 32-char lowercase hex FUID.
        // Anything else is a regression.
        if (p.unique_id == p.name) continue;
        REQUIRE(p.unique_id.size() == 32);
        for (char c : p.unique_id) {
            const bool is_hex = (c >= '0' && c <= '9')
                             || (c >= 'a' && c <= 'f');
            REQUIRE(is_hex);
        }
        fuid_shaped++;
    }
    // No strict "must find FUID" assertion — plugin_slot_vst3 links
    // live behind PULP_HAS_VST3 and CI hosts vary. If any VST3 opens
    // at all, fuid_shaped should be > 0; the loop above fails loudly
    // on malformed IDs either way.
    if (total_vst3 > 0) {
        SUCCEED("scanned " << total_vst3 << " VST3 plugin(s), "
                           << fuid_shaped << " with FUID-shaped unique_id");
    } else {
        SUCCEED("no VST3 plugins installed — structural test skipped");
    }
}

// ── PluginSlot tests ────────────────────────────────────────────────────

TEST_CASE("PluginSlot load returns nullptr for stub", "[host][slot]") {
    PluginInfo info;
    info.name = "TestPlugin";
    info.path = "/nonexistent/path.vst3";
    info.format = PluginFormat::VST3;

    auto slot = PluginSlot::load(info);
    REQUIRE(slot == nullptr); // Stub always returns nullptr
}

TEST_CASE("PluginSlot load fails cleanly for invalid dispatch inputs",
          "[host][slot]") {
    PluginInfo clap;
    clap.name = "MissingClap";
    clap.path = "/definitely/missing/Missing.clap";
    clap.format = PluginFormat::CLAP;
    REQUIRE(PluginSlot::load(clap) == nullptr);

    PluginInfo au;
    au.name = "BadAU";
    au.unique_id = "not-a-4cc-triplet";
    au.format = PluginFormat::AudioUnit;
    REQUIRE(PluginSlot::load(au) == nullptr);

    PluginInfo auv3 = au;
    auv3.name = "BadAUv3";
    auv3.format = PluginFormat::AudioUnitV3;
    REQUIRE(PluginSlot::load(auv3) == nullptr);
}

// #296 regression: set_parameter on the CLAP slot must be observable via
// get_parameter immediately (cached readback), not silently dropped.
// Unknown param IDs must be rejected instead of polluting the cache.
//
// If a built pulpsynth.clap isn't findable the test skips gracefully.
TEST_CASE("ClapSlot::set_parameter round-trip via get_parameter",
          "[host][slot][clap][issue-296]") {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates = {
        fs::current_path() / "examples" / "pulpsynth" / "pulpsynth.clap",
        fs::current_path().parent_path() / "examples" / "pulpsynth" / "pulpsynth.clap",
    };
    fs::path found;
    for (const auto& p : candidates) {
        std::error_code ec;
        if (fs::exists(p, ec)) { found = p; break; }
    }
    if (found.empty()) {
        SUCCEED("pulpsynth.clap not built — skipping CLAP set_parameter integration");
        return;
    }

    PluginInfo info;
    info.name = "pulpsynth";
    info.path = found.string();
    info.format = PluginFormat::CLAP;

    auto slot = PluginSlot::load(info);
    if (!slot) {
        SUCCEED("CLAP slot load returned nullptr at this path");
        return;
    }

    auto params = slot->parameters();
    if (params.empty()) {
        SUCCEED("plugin exposes no parameters — cannot exercise set_parameter");
        return;
    }

    const auto pid = params.front().id;

    SECTION("set_parameter round-trips via get_parameter") {
        slot->set_parameter(pid, 0.25f);
        REQUIRE_THAT(slot->get_parameter(pid), WithinAbs(0.25f, 1e-6f));
        slot->set_parameter(pid, 0.75f);
        REQUIRE_THAT(slot->get_parameter(pid), WithinAbs(0.75f, 1e-6f));
    }

    SECTION("unknown param IDs are rejected, not cached") {
        const uint32_t bogus_id = 0xDEADBEEF;
        const float before = slot->get_parameter(bogus_id);
        slot->set_parameter(bogus_id, 99.0f);
        const float after = slot->get_parameter(bogus_id);
        // bogus_id rejected — readback must not reflect 99.0f.
        REQUIRE_THAT(after, WithinAbs(before, 1e-6f));
    }
}

#ifdef PULP_TEST_CLAP_PATH
// PulpGain.clap is Pulp's own alloc-free gain effect, so any heap allocation
// during slot->process() is attributable to the host-side ClapSlot wrapper —
// exactly the in_ptrs_/out_ptrs_/in_event_storage_ vectors that prepare() now
// reserves. Asserts a steady-state block allocates zero times.
TEST_CASE("ClapSlot::process is allocation-free after prepare() reserves",
          "[host][slot][clap][rt-safety][no-alloc]") {
    namespace fs = std::filesystem;
    if (!fs::exists(PULP_TEST_CLAP_PATH)) {
        SUCCEED("PulpGain.clap not built — skipping ClapSlot process no-alloc test");
        return;
    }

    PluginInfo info;
    info.name = "PulpGain";
    info.path = PULP_TEST_CLAP_PATH;
    info.format = PluginFormat::CLAP;

    auto slot = PluginSlot::load(info);
    if (!slot) {
        SUCCEED("PulpGain.clap not loadable in this environment");
        return;
    }

    constexpr int kFrames = 128;
    REQUIRE(slot->prepare(48000.0, kFrames));

    std::vector<float> l_in(kFrames, 0.25f), r_in(kFrames, 0.25f);
    std::vector<float> l_out(kFrames, 0.0f), r_out(kFrames, 0.0f);
    std::array<const float*, 2> in_ch{l_in.data(), r_in.data()};
    std::array<float*, 2> out_ch{l_out.data(), r_out.data()};
    pulp::audio::BufferView<const float> in_view(in_ch.data(), 2, kFrames);
    pulp::audio::BufferView<float> out_view(out_ch.data(), 2, kFrames);

    // Populate parameter + MIDI events so the first block also exercises the
    // in_event_storage_ emplace path, not just the channel-pointer vectors.
    ParameterEventQueue param_events;
    const auto params = slot->parameters();
    if (!params.empty()) {
        param_events.push(pulp::state::ParameterEvent{
            .param_id = params.front().id, .sample_offset = 0, .value = 0.5f});
    }
    pulp::midi::MidiBuffer midi_in;
    midi_in.add(pulp::midi::MidiEvent::note_on(0, 60, 100));
    midi_in.add(pulp::midi::MidiEvent::cc(0, 1, 64));
    pulp::midi::MidiBuffer midi_out;

    // Measure the FIRST block after prepare(), deliberately with no warm-up:
    // that is exactly where the prepare()-time reserve pays off. Without it,
    // the first in_ptrs_/out_ptrs_ resize (from capacity 0) and the first
    // in_event_storage_ emplace would each grow-and-allocate; a warmed-up
    // block would hide that because the vectors would already hold capacity.
    // PulpGain's gain process() is itself allocation-free, so any allocation
    // here is attributable to the host-side ClapSlot wrapper.
    std::size_t allocation_count = 0;
    {
        pulp::test::RtAllocationProbe probe;
        slot->process(out_view, in_view, midi_in, midi_out, param_events, kFrames);
        allocation_count = probe.allocation_count();
    }

    REQUIRE(allocation_count == 0);
}
#endif  // PULP_TEST_CLAP_PATH

// #297 regression: VST3 set_parameter must mirror to the controller AND
// queue an edit for processor delivery in the next block. The processor
// side requires a live VST3 plugin to exercise, so this test walks the
// system install folders for any .vst3 plugin and gracefully skips when
// none exists. The controller-mirror half is observable independently:
// set → get returns the written value.
TEST_CASE("VST3 set_parameter -> get_parameter controller-mirror round-trip",
          "[host][slot][vst3][issue-297]") {
    namespace fs = std::filesystem;
    std::vector<fs::path> search_roots = {
#if defined(__APPLE__)
        fs::path(std::getenv("HOME") ? std::getenv("HOME") : "")
            / "Library/Audio/Plug-Ins/VST3",
        "/Library/Audio/Plug-Ins/VST3",
#elif defined(_WIN32)
        "C:/Program Files/Common Files/VST3",
#elif defined(__linux__)
        fs::path(std::getenv("HOME") ? std::getenv("HOME") : "")
            / ".vst3",
        "/usr/lib/vst3",
#endif
    };

    fs::path found;
    for (const auto& root : search_roots) {
        std::error_code ec;
        if (!fs::is_directory(root, ec)) continue;
        for (const auto& ent : fs::directory_iterator(root, ec)) {
            if (ent.path().extension() == ".vst3") {
                found = ent.path();
                break;
            }
        }
        if (!found.empty()) break;
    }

    if (found.empty()) {
        SUCCEED("No .vst3 plugin installed on this system — skipping VST3 set_parameter integration");
        return;
    }

    PluginInfo info;
    info.name = found.stem().string();
    info.path = found.string();
    info.format = PluginFormat::VST3;

    auto slot = PluginSlot::load(info);
    if (!slot) {
        SUCCEED("VST3 slot load returned nullptr — plugin may have rejected the host");
        return;
    }

    auto params = slot->parameters();
    if (params.empty()) {
        SUCCEED("plugin exposes no parameters — cannot exercise set_parameter");
        return;
    }

    // Pick a CONTINUOUS, writable parameter. A stepped/boolean param — the very
    // common "Bypass" toggle is frequently params.front() — legitimately
    // quantizes a mid-range set to its nearest step (e.g. 0.5 -> 1.0), which is
    // correct plugin behavior, not a controller-mirror failure. Skip stepped,
    // bypass, and read-only params so the round-trip exercises real precision.
    const HostParamInfo* continuous = nullptr;
    for (const auto& p : params) {
        if (p.flags.stepped || p.flags.is_bypass || p.flags.read_only) continue;
        if (p.max_value <= p.min_value) continue;
        continuous = &p;
        break;
    }
    if (!continuous) {
        SUCCEED("plugin exposes no continuous writable parameter — "
                "cannot exercise a precise set_parameter round-trip");
        return;
    }

    const auto& pinfo = *continuous;
    const auto pid = pinfo.id;
    const float target = (pinfo.min_value + pinfo.max_value) * 0.5f;

    // Read baseline, write, read back — the controller mirror must reflect
    // the set immediately even though the processor sees it next block.
    const float before = slot->get_parameter(pid);
    slot->set_parameter(pid, target);
    const float after = slot->get_parameter(pid);

    // Normalized round-trip can lose precision; be tolerant.
    const float tol = std::abs(pinfo.max_value - pinfo.min_value) * 1e-3f;
    INFO("param=" << pinfo.name << " id=" << pid
         << " before=" << before << " target=" << target
         << " after=" << after << " tol=" << tol);
    REQUIRE_THAT(after, WithinAbs(target, std::max(tol, 1e-4f)));
    (void)before;
}

#include <pulp/host/graph_serializer.hpp>

TEST_CASE("GraphSerializer round-trips topology + connections + layout",
          "[host][serializer]") {
    SignalGraph g1;
    auto a = g1.add_input_node(2, "in");
    auto b = g1.add_gain_node("g");
    auto c = g1.add_output_node(2, "out");
    REQUIRE(g1.connect(a, 0, b, 0));
    REQUIRE(g1.connect(a, 1, b, 1));
    REQUIRE(g1.connect(b, 0, c, 0));
    REQUIRE(g1.connect(b, 1, c, 1));
    g1.set_node_gain(b, 0.75f);

    std::unordered_map<NodeId, std::pair<float,float>> layout = {
        {a, {10.f, 20.f}}, {b, {200.f, 20.f}}, {c, {400.f, 20.f}}
    };
    std::string json = GraphSerializer::to_json(g1, layout);
    REQUIRE(!json.empty());

    SignalGraph g2;
    auto result = GraphSerializer::from_json(g2, json);
    REQUIRE(result.ok);
    REQUIRE(result.missing_plugins.empty());
    REQUIRE(g2.nodes().size() == 3);
    REQUIRE(g2.connections().size() == 4);
    REQUIRE(result.editor_layout.size() == 3);

    // The new node ids may differ from the originals; verify by name.
    NodeId in2 = 0, g2id = 0, out2 = 0;
    for (const auto& n : g2.nodes()) {
        if (n.name == "in")  in2 = n.id;
        if (n.name == "g")   g2id = n.id;
        if (n.name == "out") out2 = n.id;
    }
    REQUIRE(in2 != 0);
    REQUIRE(g2id != 0);
    REQUIRE(out2 != 0);
    REQUIRE(std::abs(g2.node_gain(g2id) - 0.75f) < 1e-6f);
}

// ── Real CLAP loader (integration test, skipped when no test plugin built) ──

#include <filesystem>
#include <vector>

#ifdef PULP_TEST_CLAP_PATH
namespace {

constexpr uint32_t kPulpGainInputGain = 1;
constexpr uint32_t kPulpGainOutputGain = 2;
constexpr uint32_t kPulpGainBypass = 3;

std::unique_ptr<PluginSlot> load_pulpgain_clap_slot() {
    namespace fs = std::filesystem;
    const std::string path = PULP_TEST_CLAP_PATH;
    if (!fs::exists(path)) {
        WARN("CLAP test plugin not built at " << path << " - skipping");
        return nullptr;
    }

    PluginInfo info;
    info.name = "PulpGain";
    info.path = path;
    info.format = PluginFormat::CLAP;

    auto slot = PluginSlot::load(info);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->is_loaded());
    return slot;
}

const HostParamInfo* find_param(const std::vector<HostParamInfo>& params,
                                uint32_t id) {
    auto it = std::find_if(params.begin(), params.end(),
                           [id](const HostParamInfo& p) { return p.id == id; });
    return it == params.end() ? nullptr : &*it;
}

float process_first_sample(PluginSlot& slot, float input_sample) {
    constexpr int kFrames = 32;
    std::vector<float> in_l(kFrames, input_sample), in_r(kFrames, input_sample);
    std::vector<float> out_l(kFrames, 0.0f), out_r(kFrames, 0.0f);
    const float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> in(in_ptrs, 2, kFrames);
    pulp::audio::BufferView<float> out(out_ptrs, 2, kFrames);
    pulp::midi::MidiBuffer mi, mo;
    pulp::host::ParameterEventQueue pe;
    slot.process(out, in, mi, mo, pe, kFrames);
    return out_l[0];
}

} // namespace

TEST_CASE("PluginScanner CLAP descriptor surfaces PulpGain bundle metadata",
          "[host][scanner][clap][issue-493]") {
    namespace fs = std::filesystem;
    const fs::path path = PULP_TEST_CLAP_PATH;
    if (!fs::exists(path)) {
        WARN("CLAP test plugin not built at " << path << " - skipping");
        return;
    }

    PluginScanner scanner;
    ScanOptions opts;
    opts.scan_vst3 = false;
    opts.scan_au = false;
    opts.scan_lv2 = false;
    opts.scan_clap = true;
    opts.only_extra_paths = true;
    opts.extra_paths = { path.parent_path().string() };

    const auto plugins = scanner.scan(opts);
    const PluginInfo* found = nullptr;
    for (const auto& plugin : plugins) {
        if (plugin.path == path.string()) {
            found = &plugin;
            break;
        }
    }

    REQUIRE(found != nullptr);
    REQUIRE(found->path == path.string());
    REQUIRE(found->format == PluginFormat::CLAP);
    REQUIRE(found->name == "PulpGain");
    REQUIRE(found->manufacturer == "Pulp");
    REQUIRE(found->version == "1.0.0");
    REQUIRE(found->unique_id == "com.pulp.gain");
    REQUIRE_FALSE(found->unique_id.empty());
    REQUIRE(found->category == "Fx");
    REQUIRE(found->is_effect);
    REQUIRE_FALSE(found->is_instrument);
    REQUIRE(found->features.size() == 1);
    REQUIRE(found->features.front() == "audio-effect");
    REQUIRE_FALSE(found->supports_midi_in);
    REQUIRE_FALSE(found->supports_midi_out);
    REQUIRE(found->description.empty());
}

TEST_CASE("PluginSlot loads and processes a real CLAP plugin", "[host][clap][integration]") {
    namespace fs = std::filesystem;
    const std::string path = PULP_TEST_CLAP_PATH;
    if (!fs::exists(path)) {
        WARN("CLAP test plugin not built at " << path << " — skipping");
        return;
    }

    PluginInfo info;
    info.name   = "PulpGain";
    info.path   = path;
    info.format = PluginFormat::CLAP;

    auto slot = PluginSlot::load(info);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->is_loaded());
    REQUIRE(slot->prepare(48000.0, 256));

    std::vector<float> in_l(256, 0.5f), in_r(256, 0.5f);
    std::vector<float> out_l(256, 0.0f), out_r(256, 0.0f);
    const float* in_ptrs[2]  = {in_l.data(), in_r.data()};
    float*       out_ptrs[2] = {out_l.data(), out_r.data()};

    pulp::audio::BufferView<const float> in(in_ptrs, 2, 256);
    pulp::audio::BufferView<float>       out(out_ptrs, 2, 256);
    pulp::midi::MidiBuffer mi, mo;

    pulp::host::ParameterEventQueue pe;
    slot->process(out, in, mi, mo, pe, 256);

    // PulpGain is a unity-ish gain plugin; output should not be all zeros
    // after processing a non-zero input.
    float sum = 0.0f;
    for (int i = 0; i < 256; ++i) sum += std::abs(out_l[i]) + std::abs(out_r[i]);
    REQUIRE(sum > 0.0f);

    slot->release();
}

TEST_CASE("ClapSlot process delivers automation and MIDI events to CLAP",
          "[host][slot][clap][issue-493]") {
    auto slot = load_pulpgain_clap_slot();
    if (!slot) return;

    REQUIRE(slot->prepare(48000.0, 64));
    REQUIRE_FALSE(slot->is_bypassed());
    REQUIRE(slot->parameters().size() == 3);

    constexpr int kFrames = 32;
    std::vector<float> in_l(kFrames, 0.5f), in_r(kFrames, 0.25f);
    std::vector<float> out_l(kFrames, 0.0f), out_r(kFrames, 0.0f);
    const float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[2] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> in(in_ptrs, 2, kFrames);
    pulp::audio::BufferView<float> out(out_ptrs, 2, kFrames);

    pulp::host::ParameterEventQueue params;
    params.push({kPulpGainOutputGain, 0, 6.0f});
    params.push({kPulpGainInputGain, 8, 0.0f});
    REQUIRE(params.size() == 2);
    REQUIRE_FALSE(params.empty());

    pulp::midi::MidiBuffer midi_in;
    auto note = pulp::midi::MidiEvent::note_on(0, 60, 100);
    note.sample_offset = 12;
    auto program = pulp::midi::MidiEvent::program_change(0, 5);
    program.sample_offset = 4;
    midi_in.add(note);
    midi_in.add(program);
    REQUIRE(midi_in.size() == 2);
    REQUIRE_FALSE(midi_in.empty());

    pulp::midi::MidiBuffer midi_out;
    REQUIRE(midi_out.empty());
    slot->process(out, in, midi_in, midi_out, params, kFrames);

    REQUIRE(midi_out.empty());
    REQUIRE(out_l[0] > in_l[0]);
    REQUIRE(out_r[0] > in_r[0]);
    REQUIRE(std::isfinite(out_l[0]));
    REQUIRE(std::isfinite(out_r[0]));
    REQUIRE_THAT(out_l[0] / out_r[0], WithinAbs(2.0f, 1e-3f));

    slot->release();
}

TEST_CASE("ClapSlot handles zero-channel processing and unknown params",
          "[host][slot][clap][issue-493]") {
    auto slot = load_pulpgain_clap_slot();
    if (!slot) return;

    REQUIRE(slot->prepare(48000.0, 64));

    const float before = slot->get_parameter(0xFEEDu);
    slot->set_parameter(0xFEEDu, 12.0f);
    const float after = slot->get_parameter(0xFEEDu);
    REQUIRE_THAT(before, WithinAbs(0.0f, 1e-6f));
    REQUIRE_THAT(after, WithinAbs(0.0f, 1e-6f));
    REQUIRE(slot->parameters().size() == 3);

    pulp::audio::BufferView<const float> in;
    pulp::audio::BufferView<float> out;
    pulp::midi::MidiBuffer midi_in, midi_out;
    pulp::host::ParameterEventQueue params;
    REQUIRE_NOTHROW(slot->process(out, in, midi_in, midi_out, params, 0));
    REQUIRE(midi_out.empty());

    slot->release();
    REQUIRE_FALSE(slot->is_bypassed());
}

TEST_CASE("ClapSlot exposes PulpGain parameter metadata and host defaults",
          "[host][slot][clap][issue-493]") {
    auto slot = load_pulpgain_clap_slot();
    if (!slot) return;

    const auto& loaded_info = slot->info();
    REQUIRE(loaded_info.name == "PulpGain");
    REQUIRE(loaded_info.manufacturer == "Pulp");
    REQUIRE(loaded_info.version == "1.0.0");
    REQUIRE(loaded_info.unique_id == "com.pulp.gain");

    auto params = slot->parameters();
    REQUIRE(params.size() == 3);

    const auto* input = find_param(params, kPulpGainInputGain);
    REQUIRE(input != nullptr);
    REQUIRE(input->name == "Input Gain");
    REQUIRE_THAT(input->min_value, WithinAbs(-60.0f, 1e-6f));
    REQUIRE_THAT(input->max_value, WithinAbs(24.0f, 1e-6f));
    REQUIRE_THAT(input->default_value, WithinAbs(0.0f, 1e-6f));
    REQUIRE(input->flags.automatable);
    REQUIRE_FALSE(input->flags.stepped);
    REQUIRE(input->flags.rampable);
    REQUIRE_FALSE(input->flags.modulatable);

    const auto* bypass = find_param(params, kPulpGainBypass);
    REQUIRE(bypass != nullptr);
    REQUIRE(bypass->name == "Bypass");
    REQUIRE(bypass->flags.automatable);
    REQUIRE(bypass->flags.stepped);
    REQUIRE_FALSE(bypass->flags.rampable);

    REQUIRE_FALSE(slot->is_bypassed());
    REQUIRE(slot->latency_samples() == 0);
    REQUIRE(slot->tail_samples() == 0);

    // PulpGain's CLAP adapter serves clap.gui, but suppresses the whole
    // extension when the environment disables editors (CI / headless /
    // PULP_TEST_MODE). So the honest expectation tracks the environment rather
    // than being a constant — pinning either answer outright would pass in one
    // environment and fail in the other.
    const bool editor_expected = !pulp::format::detail::editor_launch_blocked_by_environment();
    REQUIRE(slot->has_editor() == editor_expected);
    // Whichever way it lands, the scanned descriptor must agree with the slot.
    REQUIRE(slot->info().has_editor == slot->has_editor());

    // The legacy void* path is not implemented for CLAP: the extension consumes
    // a parent window and has nowhere to receive one here.
    REQUIRE(slot->create_editor_view() == nullptr);
    // A null parent is refused regardless of whether an editor exists.
    REQUIRE(slot->create_hosted_editor(nullptr) == nullptr);
}

TEST_CASE("ClapSlot bypass and release paths copy input and zero extra outputs",
          "[host][slot][clap][issue-493]") {
    auto slot = load_pulpgain_clap_slot();
    if (!slot) return;

    REQUIRE(slot->prepare(48000.0, 64));
    REQUIRE(slot->prepare(44100.0, 64)); // preparing while active releases first

    constexpr int kFrames = 16;
    std::vector<float> in_l(kFrames, 0.25f), in_r(kFrames, -0.5f);
    std::vector<float> out_l(kFrames, 9.0f), out_r(kFrames, 9.0f), out_extra(kFrames, 9.0f);
    const float* in_ptrs[2] = {in_l.data(), in_r.data()};
    float* out_ptrs[3] = {out_l.data(), out_r.data(), out_extra.data()};
    pulp::audio::BufferView<const float> in(in_ptrs, 2, kFrames);
    pulp::audio::BufferView<float> out(out_ptrs, 3, kFrames);
    pulp::midi::MidiBuffer mi, mo;
    pulp::host::ParameterEventQueue pe;

    slot->set_bypass(true);
    REQUIRE(slot->is_bypassed());
    slot->process(out, in, mi, mo, pe, kFrames);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE_THAT(out_l[i], WithinAbs(in_l[i], 1e-6f));
        REQUIRE_THAT(out_r[i], WithinAbs(in_r[i], 1e-6f));
        REQUIRE_THAT(out_extra[i], WithinAbs(0.0f, 1e-6f));
    }

    std::fill(out_l.begin(), out_l.end(), 8.0f);
    std::fill(out_r.begin(), out_r.end(), 8.0f);
    std::fill(out_extra.begin(), out_extra.end(), 8.0f);
    slot->release();
    REQUIRE(slot->is_bypassed()); // bypass flag survives release
    slot->process(out, in, mi, mo, pe, kFrames);

    for (int i = 0; i < kFrames; ++i) {
        REQUIRE_THAT(out_l[i], WithinAbs(in_l[i], 1e-6f));
        REQUIRE_THAT(out_r[i], WithinAbs(in_r[i], 1e-6f));
        REQUIRE_THAT(out_extra[i], WithinAbs(0.0f, 1e-6f));
    }
}

TEST_CASE("ClapSlot restore_state supersedes cached host edits",
          "[host][slot][clap][state][issue-493]") {
    auto slot = load_pulpgain_clap_slot();
    if (!slot) return;

    REQUIRE(slot->prepare(48000.0, 64));

    slot->set_parameter(kPulpGainOutputGain, 6.0f);
    (void)process_first_sample(*slot, 0.5f); // drain the queued host edit
    auto saved = slot->save_state();
    REQUIRE_FALSE(saved.empty());

    slot->set_parameter(kPulpGainOutputGain, -60.0f);
    REQUIRE_THAT(process_first_sample(*slot, 0.5f), WithinAbs(0.0005f, 1e-4f));
    REQUIRE_THAT(slot->get_parameter(kPulpGainOutputGain), WithinAbs(-60.0f, 1e-6f));

    REQUIRE(slot->restore_state(saved));
    REQUIRE_THAT(slot->get_parameter(kPulpGainOutputGain), WithinAbs(6.0f, 1e-6f));

    const float expected = 0.5f * std::pow(10.0f, 6.0f / 20.0f);
    REQUIRE_THAT(process_first_sample(*slot, 0.5f), WithinAbs(expected, 1e-3f));
    slot->release();
}
#endif

// ── SignalGraph hot-reload race-stress test ───────────────────────────────
//
// Exercises the snapshot-publish semantic: one thread runs process() in a
// tight loop while another thread mutates the graph. No TSan assertion here
// (builds without TSan must pass), but with TSan enabled this run should be
// clean. The behavioral contract we assert is "no crash, no wild output":
// every sample in the audio thread's output buffer is either silence (post-
// invalidate, pre-reprepare) or a valid sample in [-1, 1].

TEST_CASE("SignalGraph snapshot publish is race-clean", "[host][graph][race][issue-669][flaky]") {
    SignalGraph graph;
    auto in  = graph.add_input_node(1, "in");
    auto g   = graph.add_gain_node("g");
    auto out = graph.add_output_node(1, "out");
    REQUIRE(graph.connect(in, 0, g, 0));
    REQUIRE(graph.connect(g,  0, out, 0));
    REQUIRE(graph.prepare(48000.0, 64));
    REQUIRE(graph.set_node_gain(g, 0.5f));

    std::atomic<bool> stop{false};
    std::atomic<int>  blocks{0};
    std::atomic<int>  bad_samples{0};  // REQUIREs in audio thread would throw
                                       // and take down the process; count
                                       // violations and assert on the main
                                       // thread after join.

    // Deterministic handshake mirroring the regression-suite copy of this
    // test (test_host_regression.cpp, the [issue-669] case): without an
    // explicit "audio thread reached graph.process() at least once" promise,
    // a slow shared-tenant Windows runner can complete all 200 mutation
    // cycles + the 5ms tail sleep before the just-spawned audio thread is
    // ever scheduled, leaving `blocks == 0` and tripping
    // `REQUIRE(blocks.load() > 0)`. Captured in run #24910085528 (Windows
    // github-hosted, 2026-04-24) — see issue #669. The two-promise barrier
    // closes the window without any wall-clock sleep.
    std::promise<void> audio_started;
    auto started = audio_started.get_future();
    std::promise<void> first_block_processed;
    auto first_block = first_block_processed.get_future();

    std::thread audio([&] {
        std::vector<float> in_l(64, 0.25f);
        std::vector<float> out_l(64, 0.0f);
        const float* in_ptrs[1]  = {in_l.data()};
        float*       out_ptrs[1] = {out_l.data()};
        pulp::audio::BufferView<const float> iv(in_ptrs, 1, 64);
        pulp::audio::BufferView<float>       ov(out_ptrs, 1, 64);
        audio_started.set_value();
        bool first_block_signalled = false;
        while (!stop.load(std::memory_order_relaxed)) {
            graph.process(ov, iv, 64);
            for (int i = 0; i < 64; ++i) {
                const float v = out_l[i];
                // Either silence (snapshot invalidated mid-run) or the
                // set-gain path (0.25 * 0.5 = 0.125). Anything else = race.
                if (!(v == 0.0f || v == 0.125f)) {
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

    // Wait for the audio thread to launch AND prove it has executed
    // graph.process() at least once. Both barriers run before mutation so
    // the test cannot pass or fail without exercising the snapshot-publish
    // path under contention.
    started.wait();
    first_block.wait();

    // Hammer the graph with mutation cycles from the "UI" thread.
    const int cycles = 200;
    for (int i = 0; i < cycles; ++i) {
        REQUIRE(graph.disconnect(g, 0, out, 0));
        REQUIRE(graph.connect(g, 0, out, 0));
        REQUIRE(graph.prepare(48000.0, 64));
    }

    // Let the audio thread see the final prepared state for a few blocks
    // so the test ends with the snapshot published (not nullptr).
    std::this_thread::sleep_for(std::chrono::milliseconds(5));
    stop.store(true, std::memory_order_relaxed);
    audio.join();

    REQUIRE(blocks.load() > 0);
    REQUIRE(bad_samples.load() == 0);
}

TEST_CASE("ParameterEventQueue sorts sample offsets and preserves duplicates",
          "[host][params]") {
    ParameterEventQueue queue;
    queue.push({3, 64, 0.75f});
    queue.push(ParameterEvent{1, 0, 0.25f});
    queue.push({2, 32, 0.5f});
    queue.push({4, 32, 0.6f});

    REQUIRE_FALSE(queue.empty());
    REQUIRE(queue.size() == 4);

    queue.sort();
    const auto& events = queue.events();
    REQUIRE(events[0].param_id == 1);
    REQUIRE(events[0].sample_offset == 0);
    REQUIRE(events[1].param_id == 2);
    REQUIRE(events[1].sample_offset == 32);
    REQUIRE(events[2].param_id == 4);
    REQUIRE(events[2].sample_offset == 32);
    REQUIRE(events[3].param_id == 3);
    REQUIRE(events[3].sample_offset == 64);
}

TEST_CASE("ParameterEventQueue iteration and clear expose queued values",
          "[host][params]") {
    ParameterEventQueue queue;
    queue.push({10, -4, -1.0f});
    queue.push({11, 128, 1.0f});

    int id_sum = 0;
    float value_sum = 0.0f;
    for (const auto& event : queue) {
        id_sum += static_cast<int>(event.param_id);
        value_sum += event.value;
    }

    REQUIRE(id_sum == 21);
    REQUIRE_THAT(value_sum, WithinAbs(0.0f, 1e-6f));

    queue.clear();
    REQUIRE(queue.empty());
    REQUIRE(queue.size() == 0);
    REQUIRE(queue.begin() == queue.end());
}
