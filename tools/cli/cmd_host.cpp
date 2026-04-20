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
#include <string>
#include <string_view>
#include <vector>

namespace {

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

int cmd_scan(const std::vector<std::string>& args) {
    using namespace pulp::host;

    PluginFormat requested = PluginFormat::CLAP;
    bool all_formats = true;
    for (std::size_t i = 0; i < args.size(); ++i) {
        if ((args[i] == "--format" || args[i] == "-f") && i + 1 < args.size()) {
            requested = parse_format(args[i + 1], PluginFormat::CLAP);
            all_formats = false;
            ++i;
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
        auto results = scanner.scan(opts);
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
        std::fprintf(stderr, "  (VST3/AU/LV2 loaders are not yet implemented)\n");
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
