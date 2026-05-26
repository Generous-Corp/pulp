// Real-plugin integration test runner — item 4.2 of the macOS plugin
// authoring plan (planning/2026-05-24-macos-plugin-authoring-plan.md).
//
// This file is the SCAFFOLD for driving real, third-party plugin binaries
// (Surge XT, Vital, OB-Xd, Dexed) end-to-end through Pulp's host stack:
//
//   PluginScanner::scan()    ─┐
//   PluginSlot::load()        │   each step is asserted, with rich
//   PluginSlot::prepare()     ├─► diagnostic output when a real plugin
//   PluginSlot::process()     │   fails so the failing format / binary is
//   PluginSlot::save_state()  │   immediately identifiable.
//   PluginSlot::restore_state()
//   PluginSlot::release()    ─┘
//
// Tests are OPT-IN: this translation unit only compiles when CMake is
// configured with `-DPULP_REAL_PLUGIN_TESTS=ON`. Even when compiled, an
// individual test SKIPs (prints WARN, returns) if its fixture binary is
// missing from the cache, so a developer who only ran the downloader for
// `surge-xt` sees `dexed` as a skip rather than a hard failure.
//
// The actual plugin set lives in `test/integration/real_plugins.toml`.
// That file is parsed by the lightweight built-in TOML reader below — we
// deliberately do not pull in a third-party TOML library for one test
// binary. The reader covers exactly the subset our config uses:
// `[[array.of.tables]]`, `[table.subtable]`, `key = "string"`, and
// `key = true|false`. Anything richer would be over-fitting for a
// scaffold.

#include <catch2/catch_test_macros.hpp>

#include <pulp/host/scanner.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/parameter_event_queue.hpp>
#include <pulp/audio/buffer.hpp>
#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

namespace fs = std::filesystem;
using namespace pulp::host;

namespace {

// ── Minimal TOML subset reader ────────────────────────────────────────────
//
// Just enough TOML to load `real_plugins.toml`. Not a general-purpose
// parser. Supports:
//   - Comments: `# ...` to end-of-line
//   - `[a.b.c]` tables
//   - `[[a.b]]` arrays of tables (creates a new element on each header)
//   - `key = "string"` and `key = true|false`
//
// Anything else (numbers, multiline strings, inline tables, datetimes)
// is intentionally unsupported — adding those silently would invite
// over-reach. If a future plugin entry needs them, extend this reader
// in the same PR that needs them.

struct PluginPlatform {
    std::string url;
    std::string sha256;
    std::string archive_kind;
};

struct PluginEntry {
    std::string id;
    std::string display_name;
    std::string format;
    bool is_instrument = false;
    std::string expected_name;
    std::string expected_manufacturer;
    std::string bundle_relpath;
    // Indexed by os: "macos" | "linux" | "windows".
    std::optional<PluginPlatform> macos;
    std::optional<PluginPlatform> linux_;
    std::optional<PluginPlatform> windows;
};

std::string strip(std::string_view s) {
    while (!s.empty() && (s.front() == ' ' || s.front() == '\t')) s.remove_prefix(1);
    while (!s.empty() && (s.back() == ' ' || s.back() == '\t' || s.back() == '\r'))
        s.remove_suffix(1);
    return std::string{s};
}

// Unquote a TOML-style double-quoted string. Returns the original input
// unchanged when it is not enclosed in matched double quotes (true/false
// flow through untouched so the caller can dispatch on the literal).
std::string unquote(std::string_view s) {
    if (s.size() >= 2 && s.front() == '"' && s.back() == '"')
        return std::string{s.substr(1, s.size() - 2)};
    return std::string{s};
}

PluginPlatform& ensure_platform(PluginEntry& e, std::string_view os) {
    if (os == "macos") {
        if (!e.macos) e.macos.emplace();
        return *e.macos;
    }
    if (os == "linux") {
        if (!e.linux_) e.linux_.emplace();
        return *e.linux_;
    }
    if (!e.windows) e.windows.emplace();
    return *e.windows;
}

void assign(PluginEntry& e, const std::string& table_path,
            const std::string& key, const std::string& raw_value) {
    const std::string value = unquote(raw_value);

    if (table_path.empty() || table_path == "plugins") {
        if (key == "id") e.id = value;
        else if (key == "display_name") e.display_name = value;
        else if (key == "format") e.format = value;
        else if (key == "is_instrument") e.is_instrument = (raw_value == "true");
        else if (key == "expected_name") e.expected_name = value;
        else if (key == "expected_manufacturer") e.expected_manufacturer = value;
        else if (key == "bundle_relpath") e.bundle_relpath = value;
        return;
    }

    // `plugins.platforms.<os>` style.
    constexpr std::string_view kPlatformsPrefix = "plugins.platforms.";
    if (table_path.rfind(kPlatformsPrefix, 0) == 0) {
        const std::string os = table_path.substr(kPlatformsPrefix.size());
        PluginPlatform& p = ensure_platform(e, os);
        if (key == "url") p.url = value;
        else if (key == "sha256") p.sha256 = value;
        else if (key == "archive_kind") p.archive_kind = value;
    }
}

std::vector<PluginEntry> parse_real_plugins_toml(const fs::path& path) {
    std::vector<PluginEntry> out;
    std::ifstream in(path);
    if (!in) return out;

    std::string current_table; // "" outside any [[plugins]] block
    std::string line;
    bool inside_entry = false;
    PluginEntry cur;

    auto flush = [&]() {
        if (inside_entry) {
            out.push_back(std::move(cur));
            cur = PluginEntry{};
            inside_entry = false;
        }
    };

    while (std::getline(in, line)) {
        const std::string t = strip(line);
        if (t.empty() || t.front() == '#') continue;

        if (t.front() == '[') {
            // Header. Two flavors:
            //   [[plugins]]                    — start of a new array entry
            //   [plugins.platforms.macos]      — sub-table on the current entry
            if (t.size() >= 4 && t.substr(0, 2) == "[[" && t.substr(t.size() - 2) == "]]") {
                const std::string name = t.substr(2, t.size() - 4);
                if (name == "plugins") {
                    flush();
                    inside_entry = true;
                    current_table = "plugins";
                }
            } else if (t.front() == '[' && t.back() == ']') {
                current_table = t.substr(1, t.size() - 2);
            }
            continue;
        }

        if (!inside_entry) continue;

        const auto eq = t.find('=');
        if (eq == std::string::npos) continue;
        const std::string key = strip(std::string_view{t}.substr(0, eq));
        std::string val = strip(std::string_view{t}.substr(eq + 1));
        // Trim inline comments (everything after a free-standing `#`).
        // Don't disturb `#` inside a quoted string.
        if (!val.empty() && val.front() != '"') {
            const auto hash = val.find('#');
            if (hash != std::string::npos) val = strip(val.substr(0, hash));
        }
        assign(cur, current_table, key, val);
    }
    flush();
    return out;
}

// ── Platform + cache discovery ────────────────────────────────────────────

const PluginPlatform* platform_for_host(const PluginEntry& e) {
#if defined(__APPLE__)
    return e.macos ? &*e.macos : nullptr;
#elif defined(_WIN32)
    return e.windows ? &*e.windows : nullptr;
#else
    return e.linux_ ? &*e.linux_ : nullptr;
#endif
}

PluginFormat parse_format(const std::string& s) {
    if (s == "clap") return PluginFormat::CLAP;
    if (s == "au")   return PluginFormat::AudioUnit;
    if (s == "lv2")  return PluginFormat::LV2;
    return PluginFormat::VST3;
}

fs::path cache_root() {
    // Mirrors tools/scripts/fetch_real_plugins.py — both must agree on the
    // location or the runner will not see what the downloader wrote.
    if (const char* env = std::getenv("PULP_REAL_PLUGIN_CACHE"); env && *env)
        return fs::path(env);
#ifdef _WIN32
    if (const char* home = std::getenv("LOCALAPPDATA"); home && *home)
        return fs::path(home) / "pulp" / "real-plugins";
#endif
    if (const char* home = std::getenv("HOME"); home && *home)
        return fs::path(home) / ".cache" / "pulp" / "real-plugins";
    return fs::temp_directory_path() / "pulp-real-plugins";
}

fs::path config_path() {
    // CMake injects PULP_REAL_PLUGINS_TOML to point at the source-tree
    // manifest. Falling back to the relative path keeps the runner usable
    // when invoked from the source root (e.g. an in-tree dev build).
#ifdef PULP_REAL_PLUGINS_TOML
    return fs::path(PULP_REAL_PLUGINS_TOML);
#else
    return fs::path("test/integration/real_plugins.toml");
#endif
}

struct ResolvedFixture {
    const PluginEntry* entry = nullptr;
    fs::path bundle_path;            // Empty when the fixture isn't on disk.
    std::string skip_reason;         // Set when the entry must be skipped.
};

ResolvedFixture resolve_fixture(const PluginEntry& e) {
    ResolvedFixture r;
    r.entry = &e;

    const PluginPlatform* p = platform_for_host(e);
    if (!p) {
        r.skip_reason = "no platform entry for host OS";
        return r;
    }
    if (p->sha256 == "TBD" || p->sha256.empty()) {
        r.skip_reason = "fixture sha256 not pinned yet (placeholder)";
        return r;
    }

    const fs::path bundle = cache_root() / e.id / e.bundle_relpath;
    if (!fs::exists(bundle)) {
        r.skip_reason = "fixture not downloaded (run tools/scripts/fetch_real_plugins.py)";
        return r;
    }

    r.bundle_path = bundle;
    return r;
}

// ── Audio helpers ─────────────────────────────────────────────────────────

bool process_one_block(PluginSlot& slot, int frames, float input_sample,
                       float& out_peak) {
    std::vector<float> in_l(static_cast<size_t>(frames), input_sample);
    std::vector<float> in_r(static_cast<size_t>(frames), input_sample);
    std::vector<float> out_l(static_cast<size_t>(frames), 0.0f);
    std::vector<float> out_r(static_cast<size_t>(frames), 0.0f);
    const float* in_ptrs[2]  = {in_l.data(), in_r.data()};
    float*       out_ptrs[2] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> in(in_ptrs, 2, frames);
    pulp::audio::BufferView<float>       out(out_ptrs, 2, frames);
    pulp::midi::MidiBuffer mi, mo;
    pulp::host::ParameterEventQueue pe;
    slot.process(out, in, mi, mo, pe, frames);

    out_peak = 0.0f;
    for (int i = 0; i < frames; ++i) {
        out_peak = std::max(out_peak, std::abs(out_l[i]));
        out_peak = std::max(out_peak, std::abs(out_r[i]));
    }
    return true;
}

// ── The drive-one-plugin routine ──────────────────────────────────────────
//
// Centralized so every plugin goes through identical steps. When a real
// plugin breaks, the Catch2 section name + the WARN message identify both
// the plugin and the step.

void drive_plugin(const PluginEntry& e, const fs::path& bundle) {
    INFO("plugin id=" << e.id << " format=" << e.format
                       << " bundle=" << bundle.string());

    PluginInfo info;
    info.name          = e.expected_name;
    info.manufacturer  = e.expected_manufacturer;
    info.path          = bundle.string();
    info.format        = parse_format(e.format);
    info.is_instrument = e.is_instrument;
    info.is_effect     = !e.is_instrument;

    auto slot = PluginSlot::load(info);
    REQUIRE(slot != nullptr);
    REQUIRE(slot->is_loaded());

    SECTION("descriptor surfaces expected name") {
        const auto& got = slot->info();
        if (!e.expected_name.empty() && !got.name.empty())
            REQUIRE(got.name == e.expected_name);
    }

    SECTION("prepare + process produces a non-degenerate signal") {
        REQUIRE(slot->prepare(48000.0, 512));
        float peak = 0.0f;
        REQUIRE(process_one_block(*slot, 512, 0.25f, peak));
        // Non-degenerate: either passed-through audio (effects) or
        // silence (instruments waiting on note-on). Either is fine —
        // we only fail on NaN/inf, which would show as a non-finite peak.
        REQUIRE(std::isfinite(peak));
        slot->release();
    }

    SECTION("parameters enumerate + round-trip") {
        const auto params = slot->parameters();
        // Some plugins legitimately expose zero parameters (esp. pure
        // generators). Just assert the call is callable and stable.
        for (const auto& p : params) {
            const float baseline = slot->get_parameter(p.id);
            slot->set_parameter(p.id, baseline);
            REQUIRE(slot->get_parameter(p.id) == baseline);
        }
    }

    SECTION("state save + restore round-trip") {
        const auto blob = slot->save_state();
        // A loader that doesn't yet serialize state returns empty — that's
        // OK; we only require restore_state to accept its own output.
        REQUIRE(slot->restore_state(blob));
    }
}

} // namespace

// ── Smoke: scaffold parses correctly even when no fixtures exist ──────────

TEST_CASE("real-plugin scaffold: TOML config parses", "[host][integration][real-plugins][scaffold]") {
    const fs::path cfg = config_path();
    REQUIRE(fs::exists(cfg));

    const auto entries = parse_real_plugins_toml(cfg);
    REQUIRE_FALSE(entries.empty());

    // Sanity-check the canonical free-plugin set.
    std::vector<std::string> ids;
    for (const auto& e : entries) ids.push_back(e.id);
    REQUIRE(std::find(ids.begin(), ids.end(), "surge-xt") != ids.end());
    REQUIRE(std::find(ids.begin(), ids.end(), "vital")    != ids.end());
    REQUIRE(std::find(ids.begin(), ids.end(), "obxd")     != ids.end());
    REQUIRE(std::find(ids.begin(), ids.end(), "dexed")    != ids.end());

    for (const auto& e : entries) {
        INFO("entry id=" << e.id);
        REQUIRE_FALSE(e.format.empty());
        REQUIRE_FALSE(e.bundle_relpath.empty());
        // At least one platform table must exist.
        REQUIRE((e.macos.has_value() || e.linux_.has_value() || e.windows.has_value()));
    }
}

TEST_CASE("real-plugin scaffold: fixture cache root is resolvable",
          "[host][integration][real-plugins][scaffold]") {
    const fs::path root = cache_root();
    INFO("cache root: " << root.string());
    // We don't require the cache to exist — the downloader creates it.
    // We DO require resolution to return a non-empty, absolute path so
    // the runner never accidentally probes the working directory.
    REQUIRE_FALSE(root.empty());
    REQUIRE(root.is_absolute());
}

// ── The real driver: one TEST_CASE per plugin entry ───────────────────────
//
// Each entry SKIPs (prints WARN, returns) when its fixture is not on disk
// or when its sha256 is still TBD. Once a downloader run populates the
// cache, the same binary lights up the real assertions with zero rebuild.

TEST_CASE("real-plugin: Surge XT", "[host][integration][real-plugins][surge-xt]") {
    const auto entries = parse_real_plugins_toml(config_path());
    const auto it = std::find_if(entries.begin(), entries.end(),
        [](const PluginEntry& e) { return e.id == "surge-xt"; });
    REQUIRE(it != entries.end());

    const auto r = resolve_fixture(*it);
    if (!r.skip_reason.empty()) {
        WARN("skipping Surge XT: " << r.skip_reason);
        return;
    }
    drive_plugin(*it, r.bundle_path);
}

TEST_CASE("real-plugin: Vital", "[host][integration][real-plugins][vital]") {
    const auto entries = parse_real_plugins_toml(config_path());
    const auto it = std::find_if(entries.begin(), entries.end(),
        [](const PluginEntry& e) { return e.id == "vital"; });
    REQUIRE(it != entries.end());

    const auto r = resolve_fixture(*it);
    if (!r.skip_reason.empty()) {
        WARN("skipping Vital: " << r.skip_reason);
        return;
    }
    drive_plugin(*it, r.bundle_path);
}

TEST_CASE("real-plugin: OB-Xd", "[host][integration][real-plugins][obxd]") {
    const auto entries = parse_real_plugins_toml(config_path());
    const auto it = std::find_if(entries.begin(), entries.end(),
        [](const PluginEntry& e) { return e.id == "obxd"; });
    REQUIRE(it != entries.end());

    const auto r = resolve_fixture(*it);
    if (!r.skip_reason.empty()) {
        WARN("skipping OB-Xd: " << r.skip_reason);
        return;
    }
    drive_plugin(*it, r.bundle_path);
}

TEST_CASE("real-plugin: Dexed", "[host][integration][real-plugins][dexed]") {
    const auto entries = parse_real_plugins_toml(config_path());
    const auto it = std::find_if(entries.begin(), entries.end(),
        [](const PluginEntry& e) { return e.id == "dexed"; });
    REQUIRE(it != entries.end());

    const auto r = resolve_fixture(*it);
    if (!r.skip_reason.empty()) {
        WARN("skipping Dexed: " << r.skip_reason);
        return;
    }
    drive_plugin(*it, r.bundle_path);
}
