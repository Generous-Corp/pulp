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

/// Where the generator lives, and where a run's output lands.
///
/// The app bundle carries its own copy; a source checkout is preferred when
/// present so a developer's edits are what actually runs. Overridable, because
/// a machine that keeps the toolchain elsewhere should not need a rebuild.
std::string tools_dir() {
    if (const char* env = std::getenv("FORGE_MODULAR_TOOLS"); env && *env) return env;
    const char* home = std::getenv("HOME");
    const std::string source =
        "/Volumes/Workshop/Code/pulp-modular-rack/tools/rack";
    std::error_code ec;
    if (std::filesystem::exists(std::filesystem::path(source) / "patch.py", ec))
        return source;
    return std::string(home ? home : ".") +
           "/Library/Application Support/Forge Modular/tools/rack";
}

std::string build_log_path() {
    const char* home = std::getenv("HOME");
    return std::string(home ? home : ".") +
           "/Library/Application Support/Forge Modular/last-run.log";
}

// One engine for the whole process. The shell holds a raw pointer, so it must
// outlive every editor the host opens and closes.
forge_modular::ProcessEngine& engine() {
    static forge_modular::ProcessEngine instance(tools_dir(), build_log_path());
    return instance;
}

std::unique_ptr<pulp::format::Processor> create_forge_modular() {
    auto shell = std::make_unique<forge_modular::ForgeModularShell>();
    // Without this the app had no generator at all: Build reached a null
    // engine and did nothing, while the same toolchain worked perfectly from
    // the command line. Scripted proof said nothing about the app.
    shell->set_engine(&engine());
    shell->watch_build_log(build_log_path());
    return shell;
}

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
    pulp::runtime::log_info("Forge Modular: generator {} at {}",
                            engine().available() ? "ready" : "NOT FOUND",
                            tools_dir());

    std::string screenshot_path;
    for (int i = 1; i < argc; ++i) {
        std::string arg = argv[i];
        if (arg == "--screenshot" && i + 1 < argc) screenshot_path = argv[++i];
    }

    pulp::format::StandaloneApp app(create_forge_modular);

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
