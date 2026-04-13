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
//
// This is a validation harness for Feature 5 Phase 1. It is not a DAW —
// there is no audio device I/O, no UI, no graph editor. Those land in
// later phases. The point is to prove a real third-party plugin loads
// and processes audio through Pulp's host abstraction.

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

    for (int i = 1; i < argc; ++i) {
        std::string_view a = argv[i];
        if (a == "--list") {
            list_only = true;
        } else if (a == "--path" && i + 1 < argc) {
            filter_path = argv[++i];
        } else if (a == "--id" && i + 1 < argc) {
            filter_id = argv[++i];
        } else if (a == "--help" || a == "-h") {
            std::printf("Usage: %s [--list] [--path <bundle>] [--id <clap-id>]\n", argv[0]);
            return 0;
        } else {
            std::fprintf(stderr, "Unknown arg: %s\n", argv[i]);
            return 2;
        }
    }

    if (list_only) return list_plugins();

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

    slot->process(out, in, mi, mo, kBlockSize);

    float peak = 0.f;
    for (int i = 0; i < kBlockSize; ++i) {
        peak = std::max(peak, std::max(std::abs(out_l[i]), std::abs(out_r[i])));
    }
    std::printf("Processed %d samples @ %.0f Hz. Output peak: %.4f\n",
                kBlockSize, kSampleRate, peak);

    slot->release();
    return 0;
}
