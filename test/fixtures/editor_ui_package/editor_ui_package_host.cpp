// pulp-test-editor-ui-package-host <module>=<expected-name> ...
//
// Loads every CLAP module named on the command line into this one process and
// keeps them all loaded, then reads each module's single plug-in descriptor in
// reverse load order. The fixture's processor reports in its descriptor name
// which editor factory editor_factory() returned, so each module must report
// the UI package its own pulp_add_plugin() declared: a module loaded later
// must not replace an earlier module's choice, and a module that declared
// none must report none while the others are loaded.
//
// Each UI package module is view-free (UI implies NO_EDITOR), so its only GUI
// is the editor-host contract's. The host instantiates it and opens that GUI:
// create, set_parent, a size, destroy. A module without a package must offer
// no GUI at all.

#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <clap/clap.h>
#include <clap/ext/gui.h>

#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

struct Module {
    std::string path;
    std::string expected;
    void* handle = nullptr;
    const clap_plugin_entry_t* entry = nullptr;
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

const char* platform_api() {
#if defined(__APPLE__)
    return CLAP_WINDOW_API_COCOA;
#elif defined(_WIN32)
    return CLAP_WINDOW_API_WIN32;
#else
    return CLAP_WINDOW_API_X11;
#endif
}

int g_fake_window = 0;

// Open and close the plug-in's GUI. Returns an empty string on success, or
// what went wrong.
std::string open_gui(const clap_plugin_factory_t* factory, const char* id, bool expect_gui) {
    clap_host_t host{};
    host.clap_version = CLAP_VERSION;
    host.name = "pulp-test-editor-ui-package-host";
    host.vendor = "Pulp";
    host.url = "";
    host.version = "1";
    host.get_extension = [](const clap_host_t*, const char*) -> const void* { return nullptr; };
    host.request_restart = [](const clap_host_t*) {};
    host.request_process = [](const clap_host_t*) {};
    host.request_callback = [](const clap_host_t*) {};

    const clap_plugin_t* plugin = factory->create_plugin(factory, &host, id);
    if (plugin == nullptr || !plugin->init(plugin))
        return "plug-in did not instantiate";
    std::string error;
    const auto* gui =
        static_cast<const clap_plugin_gui_t*>(plugin->get_extension(plugin, CLAP_EXT_GUI));
    if (!expect_gui) {
        if (gui != nullptr)
            error = "offers a GUI without a UI package";
    } else if (gui == nullptr) {
        error = "offers no GUI";
    } else if (!gui->is_api_supported(plugin, platform_api(), false)) {
        error = "does not support the platform window API";
    } else if (!gui->create(plugin, platform_api(), false)) {
        error = "gui create failed";
    } else {
        clap_window_t window{};
        window.api = platform_api();
#if defined(_WIN32)
        window.win32 = &g_fake_window;
#elif defined(__APPLE__)
        window.cocoa = &g_fake_window;
#else
        window.x11 = 42;
#endif
        uint32_t w = 0, h = 0;
        if (!gui->set_parent(plugin, &window))
            error = "gui set_parent failed";
        else if (!gui->get_size(plugin, &w, &h) || w != 320 || h != 200)
            error = "gui size is not the editor's 320x200";
        gui->destroy(plugin);
    }
    plugin->destroy(plugin);
    return error;
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
        m.entry = static_cast<const clap_plugin_entry_t*>(find_symbol(m.handle, "clap_entry"));
        if (m.entry == nullptr || !m.entry->init(m.path.c_str())) {
            std::fprintf(stderr, "FAIL no usable clap_entry in %s\n", m.path.c_str());
            return 1;
        }
    }

    // Reverse order: the module loaded last is read first, so a choice that
    // leaked between images would show on the earlier ones.
    for (auto it = modules.rbegin(); it != modules.rend(); ++it) {
        const auto* factory = static_cast<const clap_plugin_factory_t*>(
            it->entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
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
            continue;
        }
        const bool expect_gui = it->expected != "editor:none";
        const std::string gui_error = open_gui(factory, descriptor->id, expect_gui);
        if (!gui_error.empty()) {
            std::fprintf(stderr, "FAIL %s %s\n", it->path.c_str(), gui_error.c_str());
            ++failures;
            continue;
        }
        std::printf("ok   %s reports %s%s\n", it->path.c_str(), name,
                    expect_gui ? " and opens its contract GUI" : " and offers no GUI");
    }

    for (auto& m : modules) {
        m.entry->deinit();
        close_module(m.handle);
    }
    return failures == 0 ? 0 : 1;
}
