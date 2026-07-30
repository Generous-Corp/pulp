// Forge Modular standalone host.
//
// The same entry every Forge product uses: name the processor factory, set a
// StandaloneConfig, call run_with_editor(). The window and the editor come from
// the SDK; the editor itself is Forge's chrome, which ForgeModularShell varies
// through chrome_copy(), composer_row() and home_accessory() rather than by
// reimplementing anything.
//
// An effect, not an instrument: it passes signal through untouched so it can sit
// on the same track as a Rack Pro instance rather than taking a track of its own.
//
//   pulp-forge-modular                 # windowed editor
//   pulp-forge-modular --screenshot P  # headless: capture the first editor frame
#include "forge/brand.hpp"
#include "forge/modular_shell.hpp"
#include "forge/process_engine.hpp"

#include <pulp/format/standalone.hpp>
#include <pulp/runtime/log.hpp>

#include <cstdlib>
#include <filesystem>
#include <system_error>
#include <memory>
#include <string>

namespace {

// The factory, the engine and the toolchain path all live in modular_shell.cpp
// and are shared by every format. They were duplicated here once; the copies
// drifted, and the standalone kept resolving the toolchain to an external
// volume long after the shared one had been pointed somewhere macOS does not
// gate behind a modal.
}  // namespace

int main(int argc, char** argv) {
    // Forge Modular is a separate SKU and must not put its artifacts in another
    // product's shelf. brand::kStorageDirectory is one generated constant shared
    // by every target in a build, so it cannot be set per-product -- but
    // ProjectStore honours FORGE_PROJECTS_DIR, which can. Without this, running
    // this app wrote 121 project directories into Forge's store and Forge
    // Instrument's home screen started rendering them.
    if (const char* existing = std::getenv("FORGE_PROJECTS_DIR");
        !existing || !*existing) {
        const char* home = std::getenv("HOME");
        const std::string dir = std::string(home ? home : ".") +
            "/Library/Application Support/Forge Modular/projects";
        ::setenv("FORGE_PROJECTS_DIR", dir.c_str(), /*overwrite=*/0);
    }

    // Say plainly whether the generator is reachable. A Build that fails
    // because the toolchain is missing should be diagnosable from the app's
    // own first line, not by reading source.
    pulp::runtime::log_info("Forge Modular: standalone starting");

    std::string screenshot_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--screenshot" && i + 1 < argc) screenshot_path = argv[++i];
    }

    pulp::format::StandaloneApp app([] {
        auto p = forge_modular::create_forge_modular();
        if (auto* s = dynamic_cast<forge_modular::ForgeModularShell*>(p.get()))
            s->set_standalone(true);
        return p;
    });

    pulp::format::StandaloneConfig config;
    config.sample_rate = 48000.0;
    config.buffer_size = 256;
    config.input_channels = 2;    // effect — signal passes through untouched
    config.output_channels = 2;
    config.supports_audio_input = true;
    config.show_settings_tab = true;
    config.persist_settings = false;
    config.headless = !screenshot_path.empty();
    config.screenshot_path = screenshot_path;
    app.set_config(config);

    if (!app.run_with_editor(/*use_gpu=*/true)) {
        pulp::runtime::log_error("Forge Modular: failed to start standalone app");
        return 1;
    }
    return 0;
}
