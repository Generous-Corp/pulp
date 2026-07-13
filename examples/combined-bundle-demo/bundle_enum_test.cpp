// Artifact test for the combined-bundle demo: dlopen the BUILT .clap and prove
// the single module entry exposes TWO addressable plugins. This validates the
// pulp_add_plugin_bundle() CMake surface end-to-end (a real loadable binary),
// complementary to test_clap_bundle_entry.cpp which proves the macros at the
// API level. The .clap path is injected via PULP_BUNDLE_CLAP_BINARY.

#include <clap/clap.h>

#include <cstring>
#include <dlfcn.h>
#include <cstdio>

#ifndef PULP_BUNDLE_CLAP_BINARY
#error "PULP_BUNDLE_CLAP_BINARY must point at the built CLAP binary"
#endif

int main() {
    void* handle = dlopen(PULP_BUNDLE_CLAP_BINARY, RTLD_NOW | RTLD_LOCAL);
    if (!handle) {
        std::fprintf(stderr, "dlopen failed: %s\n", dlerror());
        return 1;
    }

    auto* entry = static_cast<const clap_plugin_entry_t*>(dlsym(handle, "clap_entry"));
    if (!entry) {
        std::fprintf(stderr, "clap_entry symbol not found\n");
        return 1;
    }

    if (!entry->init(PULP_BUNDLE_CLAP_BINARY)) {
        std::fprintf(stderr, "clap_entry.init failed\n");
        return 1;
    }

    auto* factory = static_cast<const clap_plugin_factory_t*>(
        entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
    if (!factory) {
        std::fprintf(stderr, "no plugin factory\n");
        return 1;
    }

    const uint32_t count = factory->get_plugin_count(factory);
    if (count != 2) {
        std::fprintf(stderr, "expected 2 plugins from the bundle, got %u\n", count);
        return 1;
    }

    // Both declared ids are present and addressable.
    bool saw_gain = false, saw_width = false;
    for (uint32_t i = 0; i < count; ++i) {
        const auto* d = factory->get_plugin_descriptor(factory, i);
        if (!d || !d->id) {
            std::fprintf(stderr, "null descriptor at %u\n", i);
            return 1;
        }
        if (std::strcmp(d->id, "com.pulp.bundle-demo.gain") == 0) saw_gain = true;
        if (std::strcmp(d->id, "com.pulp.bundle-demo.width") == 0) saw_width = true;
    }
    if (!saw_gain || !saw_width) {
        std::fprintf(stderr, "missing expected plugin id (gain=%d width=%d)\n",
                     saw_gain, saw_width);
        return 1;
    }

    entry->deinit();
    dlclose(handle);
    std::printf("OK: combined bundle exposes 2 plugins from one .clap\n");
    return 0;
}
