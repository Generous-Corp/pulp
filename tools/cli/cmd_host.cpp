// pulp host / pulp scan — thin CLI wrappers around pulp::host.
//
// `pulp scan`  — walk the system plug-in paths and print what was found.
// `pulp host`  — load a .clap (today) and run a short synthetic block
//                through it. Smoke-tests the hosting pipeline without
//                requiring a DAW.

#include "cli_common.hpp"

#include <pulp/audio/buffer.hpp>
#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/scanner.hpp>
#include <pulp/midi/buffer.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <string_view>
#include <vector>

namespace {

[[noreturn]] void finish_rich_scan_process() {
    // Coverage runs need std::exit so LLVM can flush the child process's
    // profile before termination. Normal `pulp scan` keeps the _Exit path
    // that skips plugin static destructors.
    std::getenv("LLVM_PROFILE_FILE") ? std::exit(0) : std::_Exit(0);
}

pulp::host::PluginFormat parse_format(std::string_view s,
                                      pulp::host::PluginFormat fallback) {
    if (s == "clap" || s == "CLAP") return pulp::host::PluginFormat::CLAP;
    if (s == "vst3" || s == "VST3") return pulp::host::PluginFormat::VST3;
    if (s == "au"   || s == "AU")   return pulp::host::PluginFormat::AudioUnit;
    if (s == "auv3" || s == "AUv3") return pulp::host::PluginFormat::AudioUnitV3;
    if (s == "lv2"  || s == "LV2")  return pulp::host::PluginFormat::LV2;
    return fallback;
}

const char* format_name(pulp::host::PluginFormat f) {
    using F = pulp::host::PluginFormat;
    switch (f) {
        case F::CLAP:         return "CLAP";
        case F::VST3:         return "VST3";
        case F::AudioUnit:    return "AU";
        case F::AudioUnitV3:  return "AUv3";
        case F::LV2:          return "LV2";
    }
    return "?";
}

} // namespace

// Filesystem-only enumeration helper used by `pulp scan --no-load`.
// Mirrors what `experimental/pulp-rs/src/cmd/scan.rs` does — walk the
// platform's standard plug-in directories for the given format,
// collect bundle paths, and derive plugin names from the bundle stem.
// No `dlopen`, no JSON parse, no plugin static-init code runs. This
// is the safe-fast path that can't ever crash on a bad neighbor.
//
// Returns the same `PluginInfo` shape as the rich (load-on-scan)
// path so the print loop downstream is identical, but every entry's
// `name` is filename-derived and `unique_id` / `manufacturer` /
// `version` are intentionally empty.
static std::vector<pulp::host::PluginInfo>
enumerate_plugins_metadata_only(pulp::host::PluginFormat format) {
    using namespace pulp::host;
    std::vector<PluginInfo> out;
    const char* extension = nullptr;
    switch (format) {
        case PluginFormat::CLAP:        extension = ".clap";      break;
        case PluginFormat::VST3:        extension = ".vst3";      break;
        case PluginFormat::AudioUnit:   extension = ".component"; break;
        case PluginFormat::AudioUnitV3: extension = ".appex";     break;
        case PluginFormat::LV2:         extension = ".lv2";       break;
    }
    if (!extension) return out;

    for (const auto& root : PluginScanner::default_paths(format)) {
        std::error_code ec;
        if (!fs::exists(root, ec) || !fs::is_directory(root, ec)) continue;
        for (auto it = fs::directory_iterator(root, ec);
             it != fs::directory_iterator{}; it.increment(ec)) {
            if (ec) break;
            const auto& p = it->path();
            if (p.extension() != extension) continue;
            PluginInfo info;
            info.path = p.string();
            info.format = format;
            info.name = p.stem().string();
            info.unique_id = info.name;
            out.push_back(std::move(info));
        }
    }
    // Sort for stable output (matches the rich path's sort order).
    std::sort(out.begin(), out.end(),
              [](const PluginInfo& a, const PluginInfo& b) {
                  return a.name < b.name;
              });
    return out;
}

int cmd_scan(const std::vector<std::string>& args) {
    using namespace pulp::host;

    // Honor --help / -h BEFORE doing any plugin enumeration. Without
    // this gate `pulp scan --help` walks the system plug-in paths and
    // dlopens every CLAP/VST3 bundle it finds — slow, noisy with
    // objc duplicate-class warnings, and fatal on any installed
    // plugin whose init code throws (caught by the cross-binary
    // parity probe; tracked as #812 for the deeper dlopen-time
    // crash). A printed usage line is what every other `pulp <cmd>
    // --help` does and what users expect.
    for (const auto& a : args) {
        if (a == "--help" || a == "-h") {
            std::printf("Usage: pulp scan [--format clap|vst3|au|auv3|lv2] [--no-load]\n");
            std::printf("\nWalks the system plug-in paths and prints discovered\n");
            std::printf("plugins. Without --format, all formats are scanned.\n");
            std::printf("\n  --no-load  Filesystem-only enumeration (#812). Names are\n");
            std::printf("             filename-derived; vendor/version/unique-id are\n");
            std::printf("             omitted. Cannot crash on a malformed plugin.\n");
            return 0;
        }
    }

    PluginFormat requested = PluginFormat::CLAP;
    bool all_formats = true;
    bool no_load = false;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "--format" || args[i] == "-f") && i + 1 < args.size()) {
            requested = parse_format(args[i + 1], PluginFormat::CLAP);
            all_formats = false;
            ++i;
        } else if (args[i] == "--no-load") {
            no_load = true;
        }
    }

    const PluginFormat formats[] = {
        PluginFormat::CLAP,  PluginFormat::VST3,
        PluginFormat::AudioUnit, PluginFormat::AudioUnitV3,
        PluginFormat::LV2,
    };

    PluginScanner scanner;
    int total = 0;
    for (PluginFormat f : formats) {
        if (!all_formats && f != requested) continue;

        std::vector<PluginInfo> results;
        if (no_load) {
            // #812 escape hatch — pure filesystem walk, no dlopen.
            // Mirrors the Rust port's default scan behaviour. Users
            // hitting plugin static-init crashes on the rich path can
            // fall back here and still discover their installed
            // bundles (without vendor/version/uid metadata).
            results = enumerate_plugins_metadata_only(f);
        } else {
            ScanOptions opts;
            opts.scan_vst3 = (f == PluginFormat::VST3);
            // AU and AUv3 share the single AudioComponent discovery path
            // (scanner_au.mm), so scan_au covers both. The actual
            // PluginInfo.format is already tagged per-entry by
            // scanner_au.mm's infer_format(), which lets us narrow the
            // results to exactly AU v2 or exactly AUv3 below. Codex P2 on
            // PR #531 / #500: without that post-scan filter, a user who
            // asked for `--format au` still got mixed AU/AUv3 results,
            // contradicting the docs and making plugin selection
            // unreliable.
            opts.scan_au   = (f == PluginFormat::AudioUnit || f == PluginFormat::AudioUnitV3);
            opts.scan_clap = (f == PluginFormat::CLAP);
            opts.scan_lv2  = (f == PluginFormat::LV2);
            results = scanner.scan(opts);
        }
        // Narrow AU / AUv3 results to the exact requested flavour. When
        // `all_formats` is in play each iteration's `f` is authoritative
        // for the bucket, so we still trim mixed entries out of the
        // wrong bucket and print each plugin in its own [au]/[auv3]
        // section.
        if (f == PluginFormat::AudioUnit || f == PluginFormat::AudioUnitV3) {
            results.erase(
                std::remove_if(results.begin(), results.end(),
                    [&](const PluginInfo& info) { return info.format != f; }),
                results.end());
        }
        if (results.empty()) continue;
        std::printf("[%s] %zu plugin(s)\n", format_name(f), results.size());
        for (auto& info : results) {
            std::printf("  %-40s %s\n",
                        info.name.empty() ? "(unnamed)" : info.name.c_str(),
                        info.path.c_str());
        }
        total += static_cast<int>(results.size());
    }
    if (total == 0) {
        std::printf("No plugins found.\n");
    }

    // #812 deeper fix: dlopen'd CLAP plugins keep their dylib loaded
    // for process lifetime (macOS dlclose is largely a no-op), and
    // their C++ static destructors run at exit. Some Pulp-built
    // plugins throw choc::json::ParseError from their bridge JSON
    // teardown, which the runtime's terminate handler converts into
    // SIGABRT — the scan output prints fine but the process aborts
    // on its way out, leaving a confusing rc=134 even on a healthy
    // scan. `_Exit` skips C++ static destructors (and atexit hooks)
    // so the process finishes with the rc we actually want.
    //
    // Trade-off: any global resources held by the runtime (Skia
    // contexts, GPU pools, Catch2 state, etc.) don't get released.
    // For `pulp scan` — a short-lived diagnostic command that owns
    // no such state past printf — that's exactly the right
    // trade-off. The skip is gated to the no-load == false path
    // (where dlopen actually happened); --no-load already
    // bypasses dlopen entirely so it doesn't need this guard.
    std::fflush(stdout);
    std::fflush(stderr);
    if (!no_load) {
        // LCOV_EXCL_START
        // _Exit terminates the process — anything past this line in a
        // unit test would never run, so it's intentionally excluded
        // from coverage. The outer behaviour (clean rc=0 on a host
        // with throw-prone CLAP plugins) is exercised end-to-end by
        // the `pulp scan` shellout test in test_cli_shellout.cpp.
        finish_rich_scan_process();
        // LCOV_EXCL_STOP
    }
    return 0;
}

int cmd_host(const std::vector<std::string>& args) {
    using namespace pulp::host;

    if (args.empty() || args[0] == "--help" || args[0] == "-h") {
        std::printf("Usage: pulp host <path/to/plugin.clap> [--format clap]\n");
        std::printf("       pulp host --id <clap-id>\n");
        std::printf("\nLoads the plug-in and runs a 256-sample synthetic block\n");
        std::printf("through it. Prints plug-in metadata and peak output level.\n");
        return args.empty() ? 1 : 0;
    }

    std::string path;
    std::string unique_id;
    PluginFormat format = PluginFormat::CLAP;
    for (std::size_t i = 0; i < args.size(); ++i) {
        const auto& a = args[i];
        if ((a == "--format" || a == "-f") && i + 1 < args.size()) {
            format = parse_format(args[i + 1], format);
            ++i;
        } else if (a == "--id" && i + 1 < args.size()) {
            unique_id = args[i + 1];
            ++i;
        } else if (path.empty() && a.size() > 0 && a[0] != '-') {
            path = a;
        }
    }

    if (path.empty()) {
        std::fprintf(stderr, "pulp host: no plug-in path given\n");
        return 1;
    }

    PluginInfo info;
    info.path      = path;
    info.format    = format;
    info.unique_id = unique_id;

    auto slot = PluginSlot::load(info);
    if (!slot) {
        std::fprintf(stderr, "pulp host: failed to load '%s'\n", path.c_str());
        switch (format) {
#if !PULP_HOST_HAS_VST3
            case PluginFormat::VST3:
                std::fprintf(stderr, "  VST3 host loader not available in this build.\n"
                                     "  Rebuild with -DPULP_HAS_VST3=ON and the VST3 SDK at external/vst3sdk.\n");
                break;
#endif
#if !PULP_HOST_HAS_AU
            case PluginFormat::AudioUnit:
            case PluginFormat::AudioUnitV3:
                // Codex 2026-04-21 wave 2 P2 on #557: the AU loader is
                // gated by AudioUnitSDK presence (`PULP_HAS_AUSDK`,
                // auto-detected when external/AudioUnitSDK exists) +
                // the APPLE platform, NOT a `PULP_HAS_AU` option —
                // that flag does not exist in the build system, so
                // the old hint was not actionable. Point users at the
                // real fix: clone the AudioUnitSDK (macOS only).
                std::fprintf(stderr, "  AU host loader not available in this build (macOS only).\n"
                                     "  Rebuild on macOS with external/AudioUnitSDK present\n"
                                     "  (git clone https://github.com/apple/AudioUnitSDK external/AudioUnitSDK).\n");
                break;
#endif
#if !PULP_HOST_HAS_LV2
            case PluginFormat::LV2:
                std::fprintf(stderr, "  LV2 host loader not available in this build.\n"
                                     "  Rebuild with -DPULP_HAS_LV2=ON.\n");
                break;
#endif
            default:
                std::fprintf(stderr, "  The plug-in bundle may be malformed, unsigned, or ABI-incompatible.\n"
                                     "  Re-scan with `pulp host scan` for a structured diagnosis.\n");
                break;
        }
        return 1;
    }

    const auto& loaded = slot->info();
    std::printf("Loaded: %s\n",
                loaded.name.empty() ? path.c_str() : loaded.name.c_str());
    if (!loaded.manufacturer.empty())
        std::printf("  vendor:  %s\n", loaded.manufacturer.c_str());
    if (!loaded.version.empty())
        std::printf("  version: %s\n", loaded.version.c_str());
    if (!loaded.unique_id.empty())
        std::printf("  id:      %s\n", loaded.unique_id.c_str());
    std::printf("  format:  %s\n", format_name(loaded.format));
    std::printf("  params:  %zu\n", slot->parameters().size());

    if (!slot->prepare(48000.0, 256)) {
        std::fprintf(stderr, "pulp host: prepare() failed\n");
        return 2;
    }

    std::vector<float> in_l(256, 0.25f), in_r(256, 0.25f);
    std::vector<float> out_l(256, 0.0f), out_r(256, 0.0f);
    const float* in_ptrs[2]  = {in_l.data(), in_r.data()};
    float*       out_ptrs[2] = {out_l.data(), out_r.data()};

    pulp::audio::BufferView<const float> in(in_ptrs, 2, 256);
    pulp::audio::BufferView<float>       out(out_ptrs, 2, 256);
    pulp::midi::MidiBuffer mi, mo;

    pulp::host::ParameterEventQueue pe;
    slot->process(out, in, mi, mo, pe, 256);

    float peak = 0.0f;
    for (int i = 0; i < 256; ++i)
        peak = std::max({peak, std::abs(out_l[i]), std::abs(out_r[i])});

    std::printf("  peak:    %.3f (256-sample block, 0.25 DC input)\n", peak);
    slot->release();
    return 0;
}
