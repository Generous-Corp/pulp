// Ask a built Pulp LV2 module to describe itself into its bundle.
//
// A host finds a plugin by reading manifest.ttl; a bundle holding only the
// shared object is invisible to every host, and invisibly so. The description
// has to come from the plugin — its port layout is its descriptor's and its
// control ports are its parameters' — so this does not generate anything
// itself. It loads the module and calls the entry the PULP_LV2_PLUGIN macro
// exports, which keeps one source of truth for what a bundle says.
//
// Build-time only, run against a module this build just produced.

#include <dlfcn.h>

#include <cstdio>
#include <cstring>

namespace {

using WriteBundleTtl = int (*)(const char*, const char*);

int fail(const char* what, const char* detail) {
    std::fprintf(stderr, "pulp-lv2-ttlgen: %s: %s\n", what, detail != nullptr ? detail : "");
    return 1;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 4) {
        std::fprintf(stderr, "usage: pulp-lv2-ttlgen <module-path> <bundle-dir> <binary-name>\n");
        return 2;
    }
    const char* module_path = argv[1];
    const char* bundle_dir = argv[2];
    const char* binary_name = argv[3];

    // RTLD_LOCAL: the module stays private to this process, so loading two
    // plugins in one build cannot cross-bind their static state.
    void* handle = dlopen(module_path, RTLD_NOW | RTLD_LOCAL);
    if (handle == nullptr)
        return fail("cannot load module", dlerror());

    // dlsym returning null is indistinguishable from a symbol whose value is
    // null, so clear the error and read it back rather than testing the result.
    (void)dlerror();
    auto* symbol = dlsym(handle, "pulp_lv2_write_bundle_ttl");
    if (const char* error = dlerror(); error != nullptr) {
        dlclose(handle);
        return fail("module exports no pulp_lv2_write_bundle_ttl", error);
    }

    WriteBundleTtl write_bundle_ttl = nullptr;
    std::memcpy(&write_bundle_ttl, &symbol, sizeof(write_bundle_ttl));
    if (write_bundle_ttl == nullptr) {
        dlclose(handle);
        return fail("pulp_lv2_write_bundle_ttl resolved to null", nullptr);
    }

    const int rc = write_bundle_ttl(bundle_dir, binary_name);
    if (rc != 0) {
        char detail[64];
        std::snprintf(detail, sizeof(detail), "code %d", rc);
        dlclose(handle);
        return fail("module could not write its bundle description", detail);
    }

    // Deliberately not dlclose()d on success: a plugin's static teardown runs
    // in a process that is about to exit anyway, and unloading has been a
    // source of at-exit crashes in modules holding thread-local state.
    return 0;
}
