// Repeated real-bundle CLAP lifecycle probe.
//
// Unlike the in-process adapter tests, this crosses dlopen, resolves the
// exported clap_entry, creates the factory product, renders one block, tears
// the instance down, and closes the image on every iteration.

#include <clap/clap.h>

#include <array>
#include <cstdio>
#include <cstring>
#include <dlfcn.h>

namespace {

const void* host_get_extension(const clap_host_t*, const char*) { return nullptr; }
void host_request_restart(const clap_host_t*) {}
void host_request_process(const clap_host_t*) {}
void host_request_callback(const clap_host_t*) {}

int fail(int cycle, const char* step, const char* detail = nullptr) {
    std::fprintf(stderr, "cycle %d: %s failed%s%s\n", cycle, step,
                 detail ? ": " : "", detail ? detail : "");
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: %s /path/to/plugin-binary\n", argv[0]);
        return 2;
    }

    constexpr int kCycles = 50;
    constexpr std::uint32_t kFrames = 64;
    const char* binary = argv[1];

    clap_host_t host{};
    host.clap_version = CLAP_VERSION;
    host.name = "Pulp bundle lifecycle probe";
    host.vendor = "Pulp";
    host.url = "https://pulp.audio";
    host.version = "1.0.0";
    host.get_extension = host_get_extension;
    host.request_restart = host_request_restart;
    host.request_process = host_request_process;
    host.request_callback = host_request_callback;

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        void* handle = dlopen(binary, RTLD_NOW | RTLD_LOCAL);
        if (!handle) return fail(cycle, "dlopen", dlerror());

        auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
        if (!entry) return fail(cycle, "dlsym(clap_entry)", dlerror());
        if (!clap_version_is_compatible(entry->clap_version))
            return fail(cycle, "CLAP version compatibility");
        if (!entry->init(binary)) return fail(cycle, "entry init");

        auto* factory = static_cast<const clap_plugin_factory_t*>(
            entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (!factory || factory->get_plugin_count(factory) == 0)
            return fail(cycle, "plugin factory");
        const clap_plugin_descriptor_t* descriptor =
            factory->get_plugin_descriptor(factory, 0);
        if (!descriptor || !descriptor->id) return fail(cycle, "plugin descriptor");

        const clap_plugin_t* plugin =
            factory->create_plugin(factory, &host, descriptor->id);
        if (!plugin) return fail(cycle, "create_plugin");
        if (!plugin->init(plugin)) return fail(cycle, "plugin init");
        if (!plugin->activate(plugin, cycle % 2 == 0 ? 44100.0 : 96000.0,
                              16, kFrames))
            return fail(cycle, "activate");
        if (!plugin->start_processing(plugin)) return fail(cycle, "start_processing");

        std::array<float, kFrames> in_l{};
        std::array<float, kFrames> in_r{};
        std::array<float, kFrames> out_l{};
        std::array<float, kFrames> out_r{};
        float* inputs[] = {in_l.data(), in_r.data()};
        float* outputs[] = {out_l.data(), out_r.data()};
        clap_audio_buffer_t input{};
        input.data32 = inputs;
        input.channel_count = 2;
        clap_audio_buffer_t output{};
        output.data32 = outputs;
        output.channel_count = 2;
        clap_process_t process{};
        process.frames_count = cycle % 2 == 0 ? 32 : kFrames;
        process.audio_inputs = &input;
        process.audio_inputs_count = 1;
        process.audio_outputs = &output;
        process.audio_outputs_count = 1;
        if (plugin->process(plugin, &process) == CLAP_PROCESS_ERROR)
            return fail(cycle, "process");

        plugin->stop_processing(plugin);
        plugin->deactivate(plugin);
        plugin->destroy(plugin);
        entry->deinit();
        if (dlclose(handle) != 0) return fail(cycle, "dlclose", dlerror());
    }

    std::printf("PASS: %d real-bundle CLAP lifecycle cycles\n", kCycles);
    return 0;
}
