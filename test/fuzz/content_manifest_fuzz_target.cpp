// libFuzzer entry point over the manifests a content pack carries.
//
// A downloaded pack is attacker-controlled, and three JSON documents inside it
// reach a parser before anything is installed: the pack manifest
// (pulp.package.json, read by the install preview), a sample-bank manifest
// (*.bank.json), and the plugin-side capability manifest a bundle embeds. One
// input is handed to all three, so a seed that is valid for one parser still
// drives the other two through their rejection paths. The install preview runs
// against a scratch pack directory holding only the manifest and a scratch
// data root, so no real content location is read or written.

#include <pulp/audio/sample_bank.hpp>
#include <pulp/state/content_capability_manifest.hpp>
#include <pulp/state/content_registry.hpp>

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <string>
#include <string_view>

#ifdef _WIN32
#include <process.h>
#else
#include <unistd.h>
#endif

namespace {

namespace fs = std::filesystem;

struct ScratchDirs {
    fs::path pack;
    fs::path data_root;

    ScratchDirs() {
#ifdef _WIN32
        const auto pid = static_cast<long long>(_getpid());
#else
        const auto pid = static_cast<long long>(getpid());
#endif
        const auto base =
            fs::temp_directory_path() / ("pulp-content-manifest-fuzz-" + std::to_string(pid));
        pack = base / "pack";
        data_root = base / "data";
        std::error_code ec;
        fs::create_directories(pack, ec);
        fs::create_directories(data_root, ec);
    }

    ~ScratchDirs() {
        std::error_code ec;
        fs::remove_all(pack.parent_path(), ec);
    }
};

const ScratchDirs& scratch() {
    static const ScratchDirs dirs;
    return dirs;
}

const pulp::state::ContentCapabilityManifest& trusted_plugin() {
    static const pulp::state::ContentCapabilityManifest plugin = [] {
        pulp::state::ContentCapabilityManifest manifest;
        manifest.plugin_id = "dev.pulp.fuzz.content-target";
        manifest.capabilities = {"content.presets.v1", "content.themes.v1", "content.samples.v1",
                                 "content.sample-banks.v1", "content.wavetables.v1"};
        manifest.content_kinds = {"presets", "themes", "samples", "sample-banks", "wavetables"};
        manifest.hot_reload_kinds = {"presets", "themes"};
        return manifest;
    }();
    return plugin;
}

bool write_manifest(const fs::path& path, std::string_view text) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(text.data(), static_cast<std::streamsize>(text.size()));
    return output.good();
}

} // namespace

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string_view text(reinterpret_cast<const char*>(data), size);

    (void)pulp::audio::parse_sample_bank_json(text);
    (void)pulp::state::parse_content_capability_manifest(text);

    const auto manifest_path = scratch().pack / "pulp.package.json";
    if (write_manifest(manifest_path, text))
        (void)pulp::state::preview_content_pack_install(scratch().pack, trusted_plugin(),
                                                        scratch().data_root);
    return 0;
}
