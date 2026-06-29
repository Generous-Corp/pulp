// Sandbox PresetManager for WebAssembly plugin builds (WAMv2 / WebCLAP).
//
// WHEN THIS IS LINKED: ONLY into WebAssembly DSP modules, in place of the native
// core/state/src/preset_manager.cpp. core/state's CMakeLists does not compile the
// `wasm/` directory; the WCLAP/WAM toolchain selects this file instead.
//
// WHY A SEPARATE TU: the native PresetManager loads and saves preset *files* from
// platform preset directories and parses them with choc's JSON layer. Two things
// make that unfit for the wasi-sdk toolchain a WebAssembly plugin is built with:
//
//   1. No exception runtime. The wasi-sdk threaded libc++/libc++abi ship without
//      `__cxa_throw` and the Itanium unwinder, so WebAssembly plugins build with
//      `-fno-exceptions`. choc's JSON header instantiates `throw` statements in
//      inline functions, which is a hard compile error under `-fno-exceptions`.
//
//   2. No host preset filesystem. A browser-hosted plugin runs in a sandbox with
//      no platform preset directory mounted. There is nothing to scan or write.
//
// So the honest behavior here is "no presets": save/load/delete fail, discovery
// returns empty. The plugin still runs; it simply exposes no preset store. Wiring
// a real preset path for WebAssembly (e.g. presets bundled in the .wclap, surfaced
// through a wasi preopen) is future work — when it lands it belongs in this file,
// not the native one.
//
// This TU is the ONLY definition of these symbols in a WebAssembly link (the
// native preset_manager.cpp is never in the same link), so there is no ODR
// conflict. Do NOT add this file to the native build.

#include <pulp/state/preset_manager.hpp>

namespace pulp::state {

PresetManager::PresetManager(StateStore& store, const std::string& manufacturer,
                             const std::string& plugin_name)
    : store_(store)
    , manufacturer_(manufacturer)
    , plugin_name_(plugin_name) {}

bool PresetManager::save(const std::string&, const std::string&) { return false; }

bool PresetManager::load(const PresetInfo&) { return false; }

bool PresetManager::load(const std::filesystem::path&) { return false; }

bool PresetManager::delete_preset(const PresetInfo&) { return false; }

bool PresetManager::rename(const PresetInfo&, const std::string&) { return false; }

std::optional<PresetInfo> PresetManager::import_file(const std::filesystem::path&) {
    return std::nullopt;
}

std::vector<PresetInfo> PresetManager::all_presets() const { return {}; }
std::vector<PresetInfo> PresetManager::factory_presets() const { return {}; }
std::vector<PresetInfo> PresetManager::user_presets() const { return {}; }
std::vector<PresetInfo> PresetManager::content_presets() const { return {}; }

void PresetManager::set_content_plugin_id(std::string plugin_id) {
    content_plugin_id_ = std::move(plugin_id);
}

void PresetManager::set_content_capabilities(std::vector<std::string> capabilities) {
    content_capabilities_ = std::move(capabilities);
}

void PresetManager::set_content_data_root(std::filesystem::path data_root) {
    content_data_root_ = std::move(data_root);
}

void PresetManager::set_content_manifest(const ContentCapabilityManifest&) {}

void PresetManager::refresh() {
    cached_presets_.clear();
    cache_valid_ = true;
}

bool PresetManager::load_next() { return false; }
bool PresetManager::load_previous() { return false; }

void PresetManager::scan_directory(const std::filesystem::path&, bool,
                                   std::vector<PresetInfo>&) const {}

void PresetManager::ensure_user_dir() {}

std::filesystem::path PresetManager::platform_presets_root() { return {}; }

} // namespace pulp::state
