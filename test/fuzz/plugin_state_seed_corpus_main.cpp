// Writes the seed corpus for the plugin-state fuzz target: one blob per
// envelope shape the restore path accepts, serialized by the same store and
// processor the target restores into. Usage: <output-dir>

#include "plugin_state_fuzz_processor.hpp"

#include <pulp/format/plugin_state_io.hpp>
#include <pulp/state/store.hpp>

#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>

namespace {

bool write_seed(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output.write(reinterpret_cast<const char*>(bytes.data()),
                 static_cast<std::streamsize>(bytes.size()));
    return output.good();
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s <output-dir>\n", argv[0]);
        return 2;
    }
    const std::filesystem::path out_dir = argv[1];
    std::error_code ec;
    std::filesystem::create_directories(out_dir, ec);

    using pulp::test::plugin_state_fuzz::FuzzProcessor;
    pulp::state::StateStore store;
    FuzzProcessor processor;
    processor.set_state_store(&store);
    processor.define_parameters(store);
    store.set_value(1, -6.0f);
    store.set_value(2, 42.0f);

    bool ok = true;
    // Legacy raw StateStore blob: no plugin payload, no envelope.
    ok &= write_seed(out_dir / "legacy-store.bin",
                     pulp::format::plugin_state_io::serialize(store, processor));
    // Versioned envelope wrapping the store and a plugin-owned payload.
    const std::string payload = "{\"preset\":\"init\",\"lfo\":[0.5,0.25]}";
    processor.plugin_state.assign(payload.begin(), payload.end());
    ok &= write_seed(out_dir / "envelope-with-payload.bin",
                     pulp::format::plugin_state_io::serialize(store, processor));
    processor.plugin_state.assign(4096, 0x5a);
    ok &= write_seed(out_dir / "envelope-large-payload.bin",
                     pulp::format::plugin_state_io::serialize(store, processor));
    return ok ? 0 : 1;
}
