// plugin-host-demo
//
// Minimal Pulp host: scans for CLAP plugins, loads one via PluginSlot::load,
// runs a block of synthetic audio through it, and prints a summary of what
// happened (plugin name, I/O, parameters, peak output).
//
// Usage:
//   pulp-plugin-host-demo                      # auto-pick first scanned CLAP
//   pulp-plugin-host-demo --list               # list scanned plugins and exit
//   pulp-plugin-host-demo --path <file.clap>   # load a specific bundle
//   pulp-plugin-host-demo --id <clap-id>       # pick a specific descriptor
//                                              # inside a multi-plugin bundle
//   pulp-plugin-host-demo --manage             # headless plugin-manager UX
//                                              # (issue #494 demo)
//
// This is a validation harness for Feature 5 Phase 1. It is not a DAW —
// there is no audio device I/O, no UI, no graph editor. Those land in
// later phases. The point is to prove a real third-party plugin loads
// and processes audio through Pulp's host abstraction.

#include <pulp/audio/buffer.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scan_blacklist.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/midi/buffer.hpp>
#include <pulp/platform/child_process.hpp>
#include <pulp/view/plugin_manager_panel.hpp>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

namespace {

using pulp::host::PluginFormat;
using pulp::host::PluginInfo;
using pulp::host::PluginScanner;
using pulp::host::PluginSlot;
using pulp::host::ScanOptions;

constexpr double kSampleRate    = 48000.0;
constexpr int    kBlockSize     = 256;
constexpr int    kNumChannels   = 2;

const char* format_name(PluginFormat f) {
    switch (f) {
        case PluginFormat::VST3:         return "VST3";
        case PluginFormat::AudioUnit:    return "AU";
        case PluginFormat::AudioUnitV3:  return "AUv3";
        case PluginFormat::CLAP:         return "CLAP";
        case PluginFormat::LV2:          return "LV2";
    }
    return "?";
}

void fill_test_signal(std::vector<float>& channel, int num_samples, double sample_rate) {
    // 440 Hz sine at -6 dBFS so the plugin has real material to process.
    const double omega = 2.0 * 3.14159265358979323846 * 440.0 / sample_rate;
    for (int i = 0; i < num_samples; ++i) {
        channel[i] = 0.5f * static_cast<float>(std::sin(omega * i));
    }
}

void print_plugin_summary(const PluginSlot& slot) {
    const auto& info = slot.info();
    std::printf("Loaded plugin:\n");
    std::printf("  Name       : %s\n", info.name.c_str());
    std::printf("  Vendor     : %s\n", info.manufacturer.c_str());
    std::printf("  Version    : %s\n", info.version.c_str());
    std::printf("  Format     : %s\n", format_name(info.format));
    std::printf("  Unique ID  : %s\n", info.unique_id.c_str());
    std::printf("  Instrument : %s\n", info.is_instrument ? "yes" : "no");
    std::printf("  I/O        : %d in → %d out\n", info.num_inputs, info.num_outputs);
    std::printf("  Latency    : %d samples\n", slot.latency_samples());
    std::printf("  Tail       : %d samples\n", slot.tail_samples());

    auto params = slot.parameters();
    std::printf("  Parameters : %zu\n", params.size());
    const size_t show = std::min<size_t>(params.size(), 8);
    for (size_t i = 0; i < show; ++i) {
        const auto& p = params[i];
        std::printf("    [%u] %s = %g (range %g..%g, default %g)\n",
                    p.id, p.name.c_str(),
                    slot.get_parameter(p.id),
                    p.min_value, p.max_value, p.default_value);
    }
    if (params.size() > show) {
        std::printf("    … %zu more\n", params.size() - show);
    }
}

int list_plugins() {
    PluginScanner scanner;
    ScanOptions opts;
    opts.scan_lv2 = false;
    auto plugins = scanner.scan(opts);
    std::printf("Scanned %zu plugins:\n", plugins.size());
    for (const auto& p : plugins) {
        std::printf("  %-5s  %-40s  id=%s\n",
                    format_name(p.format), p.name.c_str(), p.unique_id.c_str());
    }
    return 0;
}

// ── Real-world PluginManagerPanel model ─────────────────────────────────
//
// Wraps `PluginScanner` + a persistent `ScanBlacklist` behind the
// `pulp::view::PluginManagerModel` interface so the UI panel can drive
// an actual host workflow. The rescan runs on a background thread so
// the UI stays responsive — the widget polls `progress_fraction()` /
// `is_scanning()` during paint.
class LiveManagerModel : public pulp::view::PluginManagerModel {
public:
    explicit LiveManagerModel(std::filesystem::path blacklist_path)
        : blacklist_path_(std::move(blacklist_path))
    {
        blacklist_.load_from(blacklist_path_.string());
    }

    ~LiveManagerModel() override {
        if (worker_.joinable()) worker_.join();
    }

    std::vector<pulp::view::PluginManagerRow>
    rows(pulp::view::PluginManagerBucket b) const override {
        std::lock_guard<std::mutex> lk(mu_);
        switch (b) {
            case pulp::view::PluginManagerBucket::scanned:     return scanned_;
            case pulp::view::PluginManagerBucket::failed:      return failed_;
            case pulp::view::PluginManagerBucket::blacklisted: return blacklisted_snapshot();
        }
        return {};
    }

    std::vector<std::string> search_paths(PluginFormat fmt) const override {
        return PluginScanner::default_paths(fmt);
    }
    void add_search_path(PluginFormat, std::string) override {
        // The live scanner uses default_paths(); a per-user path list is
        // Workstream 03 slice 3.8 territory. Left as a no-op here so the
        // demo surface mirrors the real widget API.
    }
    void remove_search_path(PluginFormat, const std::string&) override {}

    void start_rescan() override {
        if (scanning_.exchange(true)) return;
        if (worker_.joinable()) worker_.join();

        // Codex 2026-04-21 review on #538: `ScanBlacklist` is backed by
        // an unsynchronized `unordered_map`. Passing `&blacklist_` into
        // the scan worker while the UI thread mutates the same map via
        // `set_blacklisted()` is a data race — undefined behaviour and
        // potentially a crash. Take an immutable snapshot under our own
        // mutex before handing it to the worker so the UI thread's
        // writes can't collide with the scanner's reads. The live
        // `blacklist_` is still the source of truth for persistence and
        // the next scan; this snapshot is scan-scoped.
        auto blacklist_snapshot = std::make_shared<pulp::host::ScanBlacklist>();
        {
            std::lock_guard<std::mutex> lk(mu_);
            *blacklist_snapshot = blacklist_;  // value copy
        }

        worker_ = std::thread([this, blacklist_snapshot] {
            progress_ = 0.0f;
            PluginScanner scanner;
            ScanOptions opts;
            opts.scan_lv2 = false;
            opts.blacklist = blacklist_snapshot.get();
            opts.on_progress = [this](const std::string&, int done, int total) {
                progress_ = total > 0
                    ? std::clamp(static_cast<float>(done) / static_cast<float>(total),
                                 0.0f, 1.0f)
                    : 0.0f;
            };
            auto plugins = scanner.scan(opts);

            std::lock_guard<std::mutex> lk(mu_);
            scanned_.clear();
            for (const auto& p : plugins) {
                pulp::view::PluginManagerRow row;
                row.format = p.format;
                row.name = p.name;
                row.path = p.path;
                row.last_scan_unix = std::chrono::duration_cast<std::chrono::seconds>(
                    std::chrono::system_clock::now().time_since_epoch()).count();
                scanned_.push_back(std::move(row));
            }
            progress_ = 1.0f;
            scanning_ = false;
        });
    }
    void start_rescan(const std::string& path) override {
        // Single-plugin rescan just removes any cached failure and kicks
        // the generic rescan. A richer implementation would call into
        // pulp-scan-worker for the one bundle.
        //
        // Codex 2026-04-21 wave 2 P1 on #560: `std::mutex` is NOT
        // recursive; calling `start_rescan()` while still holding `mu_`
        // self-deadlocks the UI thread (the no-arg overload takes the
        // same lock on entry via is_scanning()/worker_ manipulation).
        // Release the lock before re-entering the parameterless
        // overload so the UI thread can actually reach the scanner
        // worker.
        {
            std::lock_guard<std::mutex> lk(mu_);
            failed_.erase(std::remove_if(failed_.begin(), failed_.end(),
                [&](const auto& r) { return r.path == path; }), failed_.end());
        }
        // Fire-and-forget. Lock released above so the nested call can
        // take it again without deadlocking.
        start_rescan();
    }
    float progress_fraction() const override { return progress_.load(); }
    bool is_scanning() const override { return scanning_.load(); }

    bool is_blacklisted(const std::string& path) const override {
        std::lock_guard<std::mutex> lk(mu_);
        return blacklist_.is_blacklisted(path);
    }
    void set_blacklisted(const std::string& path, bool on,
                         const std::string& reason) override {
        {
            std::lock_guard<std::mutex> lk(mu_);
            if (on) blacklist_.blacklist(path, reason.empty() ? "user" : reason);
            else    blacklist_.clear(path);
            blacklist_.save_to(blacklist_path_.string());
        }
    }
    void reveal_in_file_manager(const std::string& path) override {
        // Codex 2026-04-21 review on #538: the earlier version built a
        // shell string by concatenating the path and ran `std::system`,
        // which breaks quoting on any legitimate path containing a
        // single quote (or worse, crafted metacharacters). Switch to
        // `pulp::platform::ChildProcess` — argv-based spawn, no shell
        // interpolation. Non-blocking fire-and-forget via the existing
        // `run` helper with a tight timeout so a hung GUI can't pin the
        // demo thread.
        using pulp::platform::ChildProcess;
        pulp::platform::ProcessOptions opts;
        opts.timeout_ms = 5'000;
#if defined(__APPLE__)
        (void)ChildProcess::run("/usr/bin/open", {"-R", path}, opts);
#elif defined(_WIN32)
        // Windows explorer.exe /select,<path> wants a single argv entry
        // with the comma; CreateProcess then passes it unchanged. No
        // shell, no interpolation.
        (void)ChildProcess::run("explorer.exe", {"/select," + path}, opts);
#else
        // xdg-open reveals the parent directory; not a perfect highlight
        // but enough for a demo. Argv-based — safe for exotic parent
        // paths.
        auto parent = std::filesystem::path(path).parent_path().string();
        (void)ChildProcess::run("xdg-open", {parent}, opts);
#endif
    }

private:
    std::vector<pulp::view::PluginManagerRow> blacklisted_snapshot() const {
        std::vector<pulp::view::PluginManagerRow> out;
        for (const auto& [path, entry] : blacklist_.entries()) {
            pulp::view::PluginManagerRow r;
            r.path = path;
            r.reason = entry.reason;
            r.last_scan_unix = entry.mtime;
            out.push_back(std::move(r));
        }
        return out;
    }

    std::filesystem::path blacklist_path_;
    mutable std::mutex mu_;
    pulp::host::ScanBlacklist blacklist_;
    std::vector<pulp::view::PluginManagerRow> scanned_, failed_;
    std::atomic<float> progress_{1.0f};
    std::atomic<bool> scanning_{false};
    std::thread worker_;
};

int manage_demo() {
    // Demonstrate the end-to-end plugin manager UX headlessly. A real
    // host would mount the `PluginManagerPanel` widget in its window.
    const auto blacklist_path =
        std::filesystem::temp_directory_path() / "pulp-host-demo-blacklist.txt";

    LiveManagerModel model(blacklist_path);
    pulp::view::PluginManagerPanel panel(model);
    panel.set_bounds({0, 0, 1200, 600});

    std::printf("Plugin manager demo — triggering rescan…\n");
    panel.trigger_rescan();

    // Poll until the background scan finishes. The widget would do this
    // naturally via repaint; here we spin briefly for the CLI output.
    for (int i = 0; i < 400 && model.is_scanning(); ++i) {
        std::this_thread::sleep_for(std::chrono::milliseconds(25));
    }
    panel.refresh();

    auto print_bucket = [&](pulp::view::PluginManagerBucket b, const char* title) {
        std::printf("\n%s (%d)\n",
                    title, panel.visible_count(b));
        for (const auto& r : panel.rows(b)) {
            std::printf("  [%-4s] %s\n    %s\n",
                        format_name(r.format),
                        r.name.empty() ? "(no name)" : r.name.c_str(),
                        r.path.c_str());
        }
    };
    print_bucket(pulp::view::PluginManagerBucket::scanned,     "Scanned");
    print_bucket(pulp::view::PluginManagerBucket::failed,      "Failed");
    print_bucket(pulp::view::PluginManagerBucket::blacklisted, "Blacklisted");

    std::printf("\nA11y labels:\n  %s\n  %s\n  %s\n",
        panel.column_access_label(pulp::view::PluginManagerBucket::scanned).c_str(),
        panel.column_access_label(pulp::view::PluginManagerBucket::failed).c_str(),
        panel.column_access_label(pulp::view::PluginManagerBucket::blacklisted).c_str());

    std::printf("\nBlacklist persisted at: %s\n", blacklist_path.string().c_str());
    return 0;
}

PluginInfo pick_plugin(const std::vector<PluginInfo>& plugins,
                       const std::string& filter_path,
                       const std::string& filter_id) {
    for (const auto& p : plugins) {
        if (!filter_path.empty() && p.path != filter_path) continue;
        if (!filter_id.empty()   && p.unique_id != filter_id) continue;
        return p;
    }
    return {};
}

}  // namespace

int main(int argc, char** argv) {
    std::string filter_path;
    std::string filter_id;
    bool list_only = false;
    bool manage_mode = false;

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--list") {
            list_only = true;
        } else if (a == "--manage") {
            manage_mode = true;
        } else if (a == "--path" && i + 1 < argc) {
            filter_path = argv[++i];
        } else if (a == "--id" && i + 1 < argc) {
            filter_id = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::printf("Usage: %s [--list] [--manage] "
                        "[--path <bundle>] [--id <clap-id>]\n", argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    if (list_only) return list_plugins();
    if (manage_mode) return manage_demo();

    PluginScanner scanner;
    ScanOptions opts;
    opts.scan_lv2 = false;
    auto plugins = scanner.scan(opts);

    PluginInfo chosen;
    if (!filter_path.empty() || !filter_id.empty()) {
        chosen = pick_plugin(plugins, filter_path, filter_id);
        if (chosen.path.empty() && !filter_path.empty()) {
            // User passed an explicit path that the scanner may not have found
            // (outside default dirs). Try it directly.
            chosen.path   = filter_path;
            chosen.format = PluginFormat::CLAP;
            chosen.name   = filter_path.substr(filter_path.find_last_of('/') + 1);
        }
    } else {
        // Prefer CLAP since that's the only format the loader implements today.
        for (const auto& p : plugins) {
            if (p.format == PluginFormat::CLAP) { chosen = p; break; }
        }
    }

    // AU plugins are identified by their OSType triplet (unique_id), not a
    // filesystem path — so treat an empty path as fine when unique_id is set.
    if (chosen.path.empty() && chosen.unique_id.empty()) {
        std::fprintf(stderr,
            "No suitable plugin found. Pass --path, --id, or --list to inspect.\n");
        return 1;
    }

    std::printf("Loading: %s  (%s)\n", chosen.name.c_str(),
                chosen.path.empty() ? chosen.unique_id.c_str() : chosen.path.c_str());

    auto slot = PluginSlot::load(chosen);
    if (!slot) {
        std::fprintf(stderr, "PluginSlot::load returned nullptr for '%s'\n",
                     chosen.name.c_str());
        return 1;
    }

    if (!slot->prepare(kSampleRate, kBlockSize)) {
        std::fprintf(stderr, "slot->prepare failed\n");
        return 1;
    }

    print_plugin_summary(*slot);

    // Build a stereo test signal and process one block.
    std::vector<float> in_l(kBlockSize),  in_r(kBlockSize);
    std::vector<float> out_l(kBlockSize, 0.f), out_r(kBlockSize, 0.f);
    fill_test_signal(in_l, kBlockSize, kSampleRate);
    fill_test_signal(in_r, kBlockSize, kSampleRate);

    const float* in_ptrs[kNumChannels]  = {in_l.data(), in_r.data()};
    float*       out_ptrs[kNumChannels] = {out_l.data(), out_r.data()};
    pulp::audio::BufferView<const float> in(in_ptrs, kNumChannels, kBlockSize);
    pulp::audio::BufferView<float>       out(out_ptrs, kNumChannels, kBlockSize);
    pulp::midi::MidiBuffer mi, mo;

    pulp::host::ParameterEventQueue pe;
    slot->process(out, in, mi, mo, pe, kBlockSize);

    float peak = 0.f;
    for (int i = 0; i < kBlockSize; ++i) {
        peak = std::max(peak, std::max(std::abs(out_l[i]), std::abs(out_r[i])));
    }
    std::printf("Processed %d samples @ %.0f Hz. Output peak: %.4f\n",
                kBlockSize, kSampleRate, peak);

    slot->release();
    return 0;
}
