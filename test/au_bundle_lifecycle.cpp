// Repeated real-bundle AU v2 lifecycle probe (macOS).
// Crosses dlopen, the exported AudioComponent factory, Open/Close (which
// constructs and destroys the Pulp processor), and dlclose every cycle.

#include <AudioToolbox/AudioToolbox.h>

#include <cstdio>
#include <dlfcn.h>

namespace {

int fail(int cycle, const char* step, const char* detail = nullptr) {
    std::fprintf(stderr, "cycle %d: %s failed%s%s\n", cycle, step,
                 detail ? ": " : "", detail ? detail : "");
    return 1;
}

}  // namespace

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr,
                     "usage: %s /path/to/component-binary FactorySymbol\n",
                     argv[0]);
        return 2;
    }
    constexpr int kCycles = 50;
    using Factory = void* (*)(const AudioComponentDescription*);

    AudioComponentDescription description{};
    description.componentType = kAudioUnitType_Effect;

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        void* handle = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
        if (!handle) return fail(cycle, "dlopen", dlerror());
        auto factory = reinterpret_cast<Factory>(dlsym(handle, argv[2]));
        if (!factory) return fail(cycle, "AudioComponent factory", dlerror());

        auto* interface = static_cast<AudioComponentPlugInInterface*>(
            factory(&description));
        if (!interface || !interface->Open || !interface->Close)
            return fail(cycle, "factory product");
        if (interface->Open(interface, nullptr) != noErr)
            return fail(cycle, "AudioComponent Open");
        if (interface->Close(interface) != noErr)
            return fail(cycle, "AudioComponent Close");
        if (dlclose(handle) != 0) return fail(cycle, "dlclose", dlerror());
    }

    std::printf("PASS: %d real-bundle AU lifecycle cycles\n", kCycles);
    return 0;
}
