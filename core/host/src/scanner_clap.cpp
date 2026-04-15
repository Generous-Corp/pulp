// CLAP bundle descriptor enumeration.
//
// A single .clap bundle can expose multiple plugins (each with its own id).
// The filesystem scanner produces one PluginInfo per descriptor instead of
// per bundle. This is needed so the loader can instantiate the correct
// plugin inside multi-plugin bundles (e.g. bundled test kits, effect suites).
//
// Falls back to a single filename-derived entry if clap_entry can't be loaded
// (missing symbol, deinit failure, etc.) so scan() stays best-effort.

#include <pulp/host/scanner.hpp>
#include <pulp/runtime/log.hpp>

#include <clap/clap.h>

#include <pulp/host/dl_shim.hpp>
#include <cstring>
#include <filesystem>
#include <vector>

namespace pulp::host {
namespace {

namespace fs = std::filesystem;

std::string resolve_clap_binary(const std::string& path) {
#if defined(__APPLE__)
    fs::path p(path);
    std::error_code ec;
    if (fs::is_directory(p, ec)) {
        auto stem = p.stem().string();
        auto inner = p / "Contents" / "MacOS" / stem;
        if (fs::exists(inner, ec)) return inner.string();
    }
#endif
    return path;
}

}  // namespace

// Called from scanner.cpp — enumerate descriptors by briefly loading
// the bundle, reading clap_plugin_factory, and extracting metadata per
// descriptor. The bundle is unloaded before return.
std::vector<PluginInfo> scan_clap_bundle_descriptors(const std::string& path) {
    std::vector<PluginInfo> results;

    auto binary = resolve_clap_binary(path);
    void* handle = dlopen(binary.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle) {
        runtime::log_warn("CLAP scan: dlopen failed for '{}': {}",
                          binary, dlerror() ? dlerror() : "unknown");
        // Fallback to filename-only entry.
        PluginInfo info;
        info.path = path;
        info.format = PluginFormat::CLAP;
        info.name = fs::path(path).stem().string();
        info.unique_id = info.name;
        results.push_back(std::move(info));
        return results;
    }

    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
    if (!entry || !entry->init || !entry->get_factory) {
        runtime::log_warn("CLAP scan: no clap_entry in '{}'", binary);
        dlclose(handle);
        PluginInfo info;
        info.path = path;
        info.format = PluginFormat::CLAP;
        info.name = fs::path(path).stem().string();
        info.unique_id = info.name;
        results.push_back(std::move(info));
        return results;
    }

    if (!entry->init(path.c_str())) {
        runtime::log_warn("CLAP scan: entry->init failed for '{}'", path);
        dlclose(handle);
        return results;
    }

    const auto* factory = static_cast<const clap_plugin_factory_t*>(
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));

    if (!factory || !factory->get_plugin_count || !factory->get_plugin_descriptor) {
        entry->deinit();
        dlclose(handle);
        return results;
    }

    const uint32_t count = factory->get_plugin_count(factory);
    results.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const auto* desc = factory->get_plugin_descriptor(factory, i);
        if (!desc) continue;

        PluginInfo info;
        info.path = path;
        info.format = PluginFormat::CLAP;
        info.name = desc->name ? desc->name : fs::path(path).stem().string();
        info.manufacturer = desc->vendor ? desc->vendor : "";
        info.version = desc->version ? desc->version : "";
        info.unique_id = desc->id ? desc->id : info.name;

        // Classify from `features` if present — CLAP declares category strings
        // like "instrument", "audio-effect", "note-effect".
        //
        // Category assignment runs in two passes so a plugin advertising
        // both `audio-effect` and `analyzer` gets the more specific
        // Analyzer label regardless of feature string order (#198 P2).
        info.is_instrument = false;
        info.is_effect = true;
        bool has_audio_effect_tag = false;
        bool has_analyzer_tag = false;
        if (desc->features) {
            for (const char* const* f = desc->features; *f; ++f) {
                info.features.emplace_back(*f);
                if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_INSTRUMENT) == 0) {
                    info.is_instrument = true;
                    info.is_effect = false;
                    info.category = "Instrument";
                } else if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_AUDIO_EFFECT) == 0) {
                    has_audio_effect_tag = true;
                } else if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_NOTE_EFFECT) == 0) {
                    info.category = "MidiEffect";
                    // #198 P2: note-effects are still effects — they process
                    // MIDI, just with no audio output. Before this fix
                    // is_effect was cleared here so existing filters that
                    // grouped by is_effect silently dropped MIDI effects.
                    info.is_effect = true;
                    info.is_instrument = false;
                    info.supports_midi_in = true;
                    info.supports_midi_out = true;
                } else if (std::strcmp(*f, CLAP_PLUGIN_FEATURE_ANALYZER) == 0) {
                    has_analyzer_tag = true;
                }
            }
        }
        // Deterministic fallback category. Analyzer takes precedence over
        // Fx when a plugin advertises both tags (matches the narrower
        // user expectation for spectrum/level-meter tools). Instrument /
        // MidiEffect already assigned above win over both.
        if (info.category.empty()) {
            if (has_analyzer_tag)          info.category = "Analyzer";
            else if (has_audio_effect_tag) info.category = "Fx";
        }
        // CLAP plugins that declare the note-ports extension produce MIDI.
        // Without extension probing we infer conservatively from features.
        info.description = desc->description ? desc->description : "";
        results.push_back(std::move(info));
    }

    entry->deinit();
    dlclose(handle);
    return results;
}

}  // namespace pulp::host
