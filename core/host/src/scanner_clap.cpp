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
// wrapped in try/catch because the plugin's static init or descriptor
// accessor can throw arbitrary C++ exceptions across the C ABI of
// CLAP. Pulp #812 surfaced exactly this: a Pulp-built plugin with a
// malformed JSON config threw `choc::json::ParseError` from inside
// its bridge code at dlopen time, the unhandled exception walked up
// past the C boundary, and `pulp scan` aborted with SIGABRT —
// killing the entire scan even though all the *other* plugins
// scanned cleanly. The catch boundary turns those crashes into
// per-plugin warnings + filename-fallback entries, so one bad
// neighbor can't take down the whole report.
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
        // Defensive #812 fallback. Triggered only when dlopen itself
        // fails — a malformed/corrupt CLAP bundle. Same family as the
        // try/catch blocks below: not reachable from a unit test that
        // doesn't ship a deliberately-broken CLAP fixture. The user-
        // visible surface is exercised by `pulp scan --no-load`
        // (test_cli_shellout.cpp [issue-812]).
        runtime::log_warn("CLAP scan: dlopen failed for '{}': {}",
                          binary, dlerror() ? dlerror() : "unknown");
        results.push_back(make_filename_fallback(path));
        return results;
    } // LCOV_EXCL_STOP

    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
    if (!entry || !entry->init || !entry->get_factory) { // LCOV_EXCL_START
        // Defensive #812 fallback — bundle dlopen'd OK but doesn't
        // expose a valid clap_entry. Same rationale as above.
        runtime::log_warn("CLAP scan: no clap_entry in '{}'", binary);
        dlclose(handle);
        results.push_back(make_filename_fallback(path));
        return results;
    } // LCOV_EXCL_STOP

    bool init_ok = false;
    try {
        init_ok = entry->init(path.c_str());
    } catch (const std::exception& e) { // LCOV_EXCL_START
        // Defensive #812 guard. Triggered only when a plugin's
        // static-init code throws across the C ABI of clap_entry —
        // not reachable from a unit test that doesn't ship its own
        // throwing CLAP fixture, so excluded from coverage. The
        // user-visible benefit is exercised by `pulp scan --no-load`
        // (see test_cli_shellout.cpp [issue-812]).
        runtime::log_warn("CLAP scan: entry->init threw for '{}': {} (#812 guard)",
                          path, e.what());
        dlclose(handle);
        results.push_back(make_filename_fallback(path));
        return results;
    } catch (...) {
        runtime::log_warn("CLAP scan: entry->init threw unknown exception for '{}' (#812 guard)",
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
        runtime::log_warn("CLAP scan: get_factory threw for '{}': {} (#812 guard)",
                          path, e.what());
    } catch (...) {
        runtime::log_warn("CLAP scan: get_factory threw unknown exception for '{}' (#812 guard)",
                          path);
    } // LCOV_EXCL_STOP

    if (!factory || !factory->get_plugin_count || !factory->get_plugin_descriptor) { // LCOV_EXCL_START
        // Defensive #812 fallback — factory pointer or its method
        // table is unusable. Same rationale as the dlopen / clap_entry
        // fallbacks above.
        try { entry->deinit(); } catch (...) {}
        dlclose(handle);
        if (results.empty()) results.push_back(make_filename_fallback(path));
        return results;
    } // LCOV_EXCL_STOP

    uint32_t count = 0;
    try {
        count = factory->get_plugin_count(factory);
    } catch (const std::exception& e) { // LCOV_EXCL_START
        runtime::log_warn("CLAP scan: get_plugin_count threw for '{}': {} (#812 guard)",
                          path, e.what());
    } catch (...) {
        runtime::log_warn("CLAP scan: get_plugin_count threw unknown exception for '{}' (#812 guard)",
                          path);
    } // LCOV_EXCL_STOP
    results.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
        const clap_plugin_descriptor_t* desc = nullptr;
        try {
            desc = factory->get_plugin_descriptor(factory, i);
        } catch (const std::exception& e) { // LCOV_EXCL_START
            runtime::log_warn("CLAP scan: get_plugin_descriptor[{}] threw for '{}': {} (#812 guard)",
                              i, path, e.what());
            continue;
        } catch (...) {
            runtime::log_warn("CLAP scan: get_plugin_descriptor[{}] threw unknown for '{}' (#812 guard)",
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

    try { entry->deinit(); } catch (...) { // LCOV_EXCL_START
        runtime::log_warn("CLAP scan: entry->deinit threw for '{}' (#812 guard)", path);
    } // LCOV_EXCL_STOP
    dlclose(handle);
    if (results.empty()) results.push_back(make_filename_fallback(path));
    return results;
}

}  // namespace pulp::host
