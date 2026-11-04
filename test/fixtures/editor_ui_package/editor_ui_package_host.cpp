// pulp-test-editor-ui-package-host <module>=<expected-name> ...
//
// Loads every CLAP module named on the command line into this one process and
// keeps them all loaded, then reads each module's single plug-in descriptor in
// reverse load order. The fixture's processor reports in its descriptor name
// which editor factory editor_factory() returned, so each module must report
// the UI package its own pulp_add_plugin() declared: a module loaded later
// must not replace an earlier module's choice, and a module that declared
// none must report none while the others are loaded.

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

// The CLAP C ABI, declared here so the host carries no SDK.
struct ClapVersion {
    std::uint32_t major, minor, revision;
};
struct ClapDescriptorHead {
    ClapVersion version;
    const char* id;
    const char* name;
};
struct ClapFactoryHead {
    std::uint32_t (*get_plugin_count)(const void* factory);
    const ClapDescriptorHead* (*get_plugin_descriptor)(const void* factory, std::uint32_t index);
};
struct ClapEntry {
    ClapVersion version;
    bool (*init)(const char* path);
    void (*deinit)();
    const void* (*get_factory)(const char* id);
};

struct Module {
    std::string path;
    std::string expected;
    void* handle = nullptr;
    const ClapEntry* entry = nullptr;
};

void* open_module(const char* path) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(LoadLibraryA(path));
#else
    return dlopen(path, RTLD_NOW | RTLD_LOCAL);
#endif
}

void* find_symbol(void* handle, const char* name) {
#if defined(_WIN32)
    return reinterpret_cast<void*>(GetProcAddress(static_cast<HMODULE>(handle), name));
#else
    return dlsym(handle, name);
#endif
}

void close_module(void* handle) {
#if defined(_WIN32)
    FreeLibrary(static_cast<HMODULE>(handle));
#else
    dlclose(handle);
#endif
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr, "usage: %s <module>=<expected-name> <module>=<expected-name>...\n",
                     argv[0]);
        return 2;
    }

    std::vector<Module> modules;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        const auto eq = arg.rfind('=');
        if (eq == std::string::npos) {
            std::fprintf(stderr, "argument without '=': %s\n", argv[i]);
            return 2;
        }
        modules.push_back({arg.substr(0, eq), arg.substr(eq + 1)});
    }

    int failures = 0;
    for (auto& m : modules) {
        m.handle = open_module(m.path.c_str());
        if (m.handle == nullptr) {
            std::fprintf(stderr, "FAIL cannot load %s\n", m.path.c_str());
            return 1;
        }
        m.entry = static_cast<const ClapEntry*>(find_symbol(m.handle, "clap_entry"));
        if (m.entry == nullptr || !m.entry->init(m.path.c_str())) {
            std::fprintf(stderr, "FAIL no usable clap_entry in %s\n", m.path.c_str());
            return 1;
        }
    }

    // Reverse order: the module loaded last is read first, so a choice that
    // leaked between images would show on the earlier ones.
    for (auto it = modules.rbegin(); it != modules.rend(); ++it) {
        const auto* factory =
            static_cast<const ClapFactoryHead*>(it->entry->get_factory("clap.plugin-factory"));
        const std::uint32_t count = factory != nullptr ? factory->get_plugin_count(factory) : 0;
        if (count != 1) {
            std::fprintf(stderr, "FAIL %s lists %u plug-ins, expected 1\n", it->path.c_str(),
                         count);
            ++failures;
            continue;
        }
        const auto* descriptor = factory->get_plugin_descriptor(factory, 0);
        const char* name =
            descriptor != nullptr && descriptor->name != nullptr ? descriptor->name : "(null)";
        if (it->expected != name) {
            std::fprintf(stderr, "FAIL %s reports %s, expected %s\n", it->path.c_str(), name,
                         it->expected.c_str());
            ++failures;
        } else {
            std::printf("ok   %s reports %s\n", it->path.c_str(), name);
        }
    }

    for (auto& m : modules) {
        m.entry->deinit();
        close_module(m.handle);
    }
    return failures == 0 ? 0 : 1;
}
