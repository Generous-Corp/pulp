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

uint32_t cap_clap_plugin_count(uint32_t count) noexcept {
    // No real CLAP bundle exposes thousands of plugins; clamp an untrusted
    // count so a malformed factory can't drive an absurd allocation.
    constexpr uint32_t kMaxClapPluginsPerBundle = 1024;
    return count > kMaxClapPluginsPerBundle ? kMaxClapPluginsPerBundle : count;
}

// Filename-only fallback used when descriptor enumeration fails for
// any reason (missing entry, init failure, exception thrown across
// the dlopen boundary). The user sees the bundle in scan output but
// without bundled metadata — better than dropping it silently and
// far better than aborting the whole scan.
PluginInfo make_filename_fallback(const std::string& path) {
    PluginInfo info;
    info.path = path;
    info.format = PluginFormat::CLAP;
    info.name = fs::path(path).stem().string();
    info.unique_id = info.name;
    return info;
}

// Called from scanner.cpp — enumerate descriptors by briefly loading
// the bundle, reading clap_plugin_factory, and extracting metadata per
// descriptor. The bundle is unloaded before return.
//
// EVERY call into the loaded bundle (entry->init, entry->get_factory,
// factory->get_plugin_count, factory->get_plugin_descriptor) is
// wrapped in try/catch because a plugin's static init or descriptor
// accessor can throw arbitrary C++ exceptions across the CLAP C ABI.
// The catch boundary turns those failures into per-plugin warnings +
// filename-fallback entries, so one bad bundle cannot take down the
// whole scan.
//
// `catch (...)` is the broad sweep deliberately: the throwing plugin
// might use a different C++ runtime than this binary (e.g. statically-
// linked libc++) and we can't reliably name its exception types.
// Worst case the unwind is incompatible and we still abort — same
// as today, no regression — but in the common case (Pulp-built
// plugins linking the shared system C++ runtime) this catches.
std::vector<PluginInfo> scan_clap_bundle_descriptors(const std::string& path) {
    std::vector<PluginInfo> results;

    auto binary = resolve_clap_binary(path);
    void* handle = dlopen(binary.c_str(), RTLD_LAZY | RTLD_LOCAL);
    if (!handle) { // LCOV_EXCL_START
        // Defensive fallback for malformed/corrupt CLAP bundles. Same family
        // as the try/catch blocks below: not reachable from a unit test that
        // doesn't ship a deliberately broken CLAP fixture. The user-visible
        // surface is exercised by `pulp scan --no-load`.
        //
        // Cache `dlerror()` in a local before formatting the warning. POSIX
        // dlerror() clears its internal state after every call, so a ternary
        // `dlerror() ? dlerror() : "..."` calls it twice and the second call
        // returns nullptr. std::format's string_view(nullptr) constructor can
        // then run strlen on null.
        const char* err = dlerror();
        runtime::log_warn("CLAP scan: dlopen failed for '{}': {}",
                          binary, err ? err : "unknown");
        results.push_back(make_filename_fallback(path));
        return results;
    } // LCOV_EXCL_STOP

    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
    if (!entry || !entry->init || !entry->get_factory) { // LCOV_EXCL_START
        // Defensive fallback: bundle dlopen'd OK but doesn't expose a valid
        // clap_entry.
        runtime::log_warn("CLAP scan: no clap_entry in '{}'", binary);
        dlclose(handle);
        results.push_back(make_filename_fallback(path));
        return results;
    } // LCOV_EXCL_STOP

    bool init_ok = false;
    try {
        init_ok = entry->init(path.c_str());
    } catch (const std::exception& e) { // LCOV_EXCL_START
        // Defensive boundary for plugins that throw across clap_entry.
        // Excluded from coverage because it requires a throwing CLAP fixture;
        // the user-visible benefit is exercised by `pulp scan --no-load`.
        runtime::log_warn("CLAP scan: entry->init threw for '{}': {}",
                          path, e.what());
        dlclose(handle);
        results.push_back(make_filename_fallback(path));
        return results;
    } catch (...) {
        runtime::log_warn("CLAP scan: entry->init threw unknown exception for '{}'",
                          path);
        dlclose(handle);
        results.push_back(make_filename_fallback(path));
        return results;
    } // LCOV_EXCL_STOP
    if (!init_ok) {
        runtime::log_warn("CLAP scan: entry->init failed for '{}'", path);
        dlclose(handle);
        return results;
    }

    const clap_plugin_factory_t* factory = nullptr;
    try {
        factory = static_cast<const clap_plugin_factory_t*>(
            entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    } catch (const std::exception& e) { // LCOV_EXCL_START
        runtime::log_warn("CLAP scan: get_factory threw for '{}': {}",
                          path, e.what());
    } catch (...) {
        runtime::log_warn("CLAP scan: get_factory threw unknown exception for '{}'",
                          path);
    } // LCOV_EXCL_STOP

    if (!factory || !factory->get_plugin_count || !factory->get_plugin_descriptor) { // LCOV_EXCL_START
        // Defensive fallback: factory pointer or its method table is unusable.
        try { entry->deinit(); } catch (...) {}
        dlclose(handle);
        if (results.empty()) results.push_back(make_filename_fallback(path));
        return results;
    } // LCOV_EXCL_STOP

    uint32_t count = 0;
    try {
        count = factory->get_plugin_count(factory);
    } catch (const std::exception& e) { // LCOV_EXCL_START
        runtime::log_warn("CLAP scan: get_plugin_count threw for '{}': {}",
                          path, e.what());
    } catch (...) {
        runtime::log_warn("CLAP scan: get_plugin_count threw unknown exception for '{}'",
                          path);
    } // LCOV_EXCL_STOP
    // A malformed bundle can RETURN (not throw) an absurd count; reserve(count)
    // would then throw bad_alloc/length_error outside the per-bundle fallback
    // and abort the whole scan. Cap it before use.
    if (uint32_t capped = cap_clap_plugin_count(count); capped != count) {
        runtime::log_warn("CLAP scan: '{}' reported {} plugins; capping at {}",
                          path, count, capped);
        count = capped;
    }
    results.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* desc = nullptr;
        try {
            desc = factory->get_plugin_descriptor(factory, i);
        } catch (const std::exception& e) { // LCOV_EXCL_START
            runtime::log_warn("CLAP scan: get_plugin_descriptor[{}] threw for '{}': {}",
                              i, path, e.what());
            continue;
        } catch (...) {
            runtime::log_warn("CLAP scan: get_plugin_descriptor[{}] threw unknown for '{}'",
                              i, path);
            continue;
        } // LCOV_EXCL_STOP
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
        // Analyzer label regardless of feature string order.
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
                    // Note-effects are still effects: they process MIDI, just
                    // with no audio output. Keep them in existing filters that
                    // group by is_effect.
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

    try { entry->deinit(); } catch (...) { // LCOV_EXCL_START
        runtime::log_warn("CLAP scan: entry->deinit threw for '{}'", path);
    } // LCOV_EXCL_STOP
    dlclose(handle);
    if (results.empty()) results.push_back(make_filename_fallback(path));
    return results;
}

}  // namespace pulp::host
