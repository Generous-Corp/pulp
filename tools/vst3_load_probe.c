// Prove a .vst3 bundle loads and publishes a factory. This is the VST3
// equivalent of the CLAP dlopen check: not a full pluginval run, but it
// catches the failures that matter at scan time -- a missing binary, an
// unresolved symbol, a bundle whose entry point never ran.
#include <dlfcn.h>
#include <stdio.h>

int main(int argc, char** argv) {
    if (argc < 2) return 2;
    void* lib = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
    if (!lib) { fprintf(stderr, "dlopen: %s\n", dlerror()); return 1; }

    // macOS VST3 bundles run bundleEntry before the factory is valid.
    typedef int (*entry_fn)(void*);
    typedef void* (*factory_fn)(void);
    entry_fn entry = (entry_fn)dlsym(lib, "bundleEntry");
    factory_fn get_factory = (factory_fn)dlsym(lib, "GetPluginFactory");

    printf("bundleEntry:      %s\n", entry ? "present" : "MISSING");
    printf("GetPluginFactory: %s\n", get_factory ? "present" : "MISSING");
    if (!entry || !get_factory) return 1;

    if (!entry(NULL)) { fprintf(stderr, "bundleEntry returned false\n"); return 1; }
    void* f = get_factory();
    printf("factory:          %s\n", f ? "returned" : "NULL");
    return f ? 0 : 1;
}
