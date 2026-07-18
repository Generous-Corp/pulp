// Repeated real-bundle VST3 lifecycle probe (macOS).
// Crosses dlopen, bundleEntry, GetPluginFactory, createInstance/release,
// bundleExit, and dlclose on every cycle.

#include <pluginterfaces/base/ipluginbase.h>
#include <pluginterfaces/vst/ivstcomponent.h>

#include <CoreFoundation/CoreFoundation.h>
#include <dlfcn.h>

#include <cstdio>
#include <cstring>

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
                     "usage: %s /path/to/plugin.vst3 /path/to/plugin-binary\n",
                     argv[0]);
        return 2;
    }
    constexpr int kCycles = 50;

    CFURLRef bundle_url = CFURLCreateFromFileSystemRepresentation(
        kCFAllocatorDefault,
        reinterpret_cast<const UInt8*>(argv[1]),
        static_cast<CFIndex>(std::strlen(argv[1])), true);
    if (!bundle_url) return fail(-1, "CFURLCreateFromFileSystemRepresentation");
    CFBundleRef bundle = CFBundleCreate(kCFAllocatorDefault, bundle_url);
    CFRelease(bundle_url);
    if (!bundle) return fail(-1, "CFBundleCreate");

    using BundleEntry = bool (*)(CFBundleRef);
    using BundleExit = bool (*)();
    using GetFactory = Steinberg::IPluginFactory* (*)();

    for (int cycle = 0; cycle < kCycles; ++cycle) {
        void* handle = dlopen(argv[2], RTLD_NOW | RTLD_LOCAL);
        if (!handle) return fail(cycle, "dlopen", dlerror());
        auto entry = reinterpret_cast<BundleEntry>(dlsym(handle, "bundleEntry"));
        auto exit = reinterpret_cast<BundleExit>(dlsym(handle, "bundleExit"));
        auto get_factory =
            reinterpret_cast<GetFactory>(dlsym(handle, "GetPluginFactory"));
        if (!entry || !exit || !get_factory)
            return fail(cycle, "required VST3 exports", dlerror());
        if (!entry(bundle)) return fail(cycle, "bundleEntry");

        Steinberg::IPluginFactory* factory = get_factory();
        if (!factory || factory->countClasses() <= 0)
            return fail(cycle, "GetPluginFactory/countClasses");
        Steinberg::PClassInfo info{};
        if (factory->getClassInfo(0, &info) != Steinberg::kResultOk)
            return fail(cycle, "getClassInfo");
        Steinberg::Vst::IComponent* component = nullptr;
        if (factory->createInstance(
                info.cid, Steinberg::Vst::IComponent::iid,
                reinterpret_cast<void**>(&component)) != Steinberg::kResultOk ||
            !component)
            return fail(cycle, "createInstance(IComponent)");
        component->release();
        factory->release();
        if (!exit()) return fail(cycle, "bundleExit");
        if (dlclose(handle) != 0) return fail(cycle, "dlclose", dlerror());
    }

    CFRelease(bundle);
    std::printf("PASS: %d real-bundle VST3 lifecycle cycles\n", kCycles);
    return 0;
}
