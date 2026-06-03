#import <Cocoa/Cocoa.h>

#include <clap/clap.h>
#include <pulp/format/clap_adapter.hpp>

#include <dlfcn.h>

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

namespace {

struct Args {
    std::string bundle_binary;
    std::string output_png;
    uint32_t width = 1000;
    uint32_t height = 600;
    int hold_seconds = 0;
    bool visible = false;
};

[[noreturn]] void usage() {
    std::cerr << "usage: pulp-elysium-ruif-plugin-capture-mac "
              << "--bundle-binary <path> --output <png> [--width N] [--height N] "
                 "[--visible] [--hold-seconds N]\n";
    std::exit(2);
}

Args parse_args(int argc, char** argv) {
    Args args;
    for (int i = 1; i < argc; ++i) {
        const std::string key = argv[i];
        auto next = [&]() -> const char* {
            if (i + 1 >= argc)
                usage();
            return argv[++i];
        };
        if (key == "--bundle-binary") {
            args.bundle_binary = next();
        } else if (key == "--output") {
            args.output_png = next();
        } else if (key == "--width") {
            args.width = static_cast<uint32_t>(std::stoul(next()));
        } else if (key == "--height") {
            args.height = static_cast<uint32_t>(std::stoul(next()));
        } else if (key == "--hold-seconds") {
            args.hold_seconds = std::stoi(next());
        } else if (key == "--visible") {
            args.visible = true;
        } else {
            usage();
        }
    }
    if (args.bundle_binary.empty() || args.output_png.empty())
        usage();
    return args;
}

void clear_no_editor_env() {
    ::unsetenv("CI");
    ::unsetenv("PULP_TEST_MODE");
    ::unsetenv("PULP_HEADLESS");
    ::unsetenv("PULP_DISABLE_PLUGIN_EDITOR");
    ::unsetenv("PULP_DISABLE_PLUGIN_GPU");
}

void pump_run_loop(int frames) {
    for (int i = 0; i < frames; ++i) {
        @autoreleasepool {
            [[NSRunLoop currentRunLoop]
                runMode:NSDefaultRunLoopMode
                beforeDate:[NSDate dateWithTimeIntervalSinceNow:0.02]];
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(8));
    }
}

bool looks_like_png(const std::vector<uint8_t>& d) {
    return d.size() > 8 && d[0] == 0x89 && d[1] == 'P' && d[2] == 'N' && d[3] == 'G';
}

void write_file(const std::string& path, const std::vector<uint8_t>& bytes) {
    std::ofstream out(path, std::ios::binary);
    if (!out)
        throw std::runtime_error("failed to open output png: " + path);
    out.write(reinterpret_cast<const char*>(bytes.data()),
              static_cast<std::streamsize>(bytes.size()));
    if (!out)
        throw std::runtime_error("failed to write output png: " + path);
}

}  // namespace

int main(int argc, char** argv) {
    clear_no_editor_env();
    const auto args = parse_args(argc, argv);

    @autoreleasepool {
        void* handle = ::dlopen(args.bundle_binary.c_str(), RTLD_NOW | RTLD_LOCAL);
        if (handle == nullptr) {
            std::cerr << "dlopen failed: " << ::dlerror() << "\n";
            return 1;
        }

        auto* entry = static_cast<const clap_plugin_entry_t*>(::dlsym(handle, "clap_entry"));
        if (entry == nullptr) {
            std::cerr << "dlsym(clap_entry) failed: " << ::dlerror() << "\n";
            ::dlclose(handle);
            return 1;
        }

        if (!entry->init(args.bundle_binary.c_str())) {
            std::cerr << "clap_entry.init failed\n";
            ::dlclose(handle);
            return 1;
        }

        const auto cleanup_entry = [&]() {
            entry->deinit();
            ::dlclose(handle);
        };

        auto* factory = static_cast<const clap_plugin_factory_t*>(
            entry->get_factory(CLAP_PLUGIN_FACTORY_ID));
        if (factory == nullptr) {
            std::cerr << "CLAP plugin factory unavailable\n";
            cleanup_entry();
            return 1;
        }

        const clap_plugin_descriptor_t* desc = factory->get_plugin_descriptor(factory, 0);
        if (desc == nullptr) {
            std::cerr << "CLAP plugin descriptor unavailable\n";
            cleanup_entry();
            return 1;
        }

        const clap_plugin_t* plugin = factory->create_plugin(factory, nullptr, desc->id);
        if (plugin == nullptr || !plugin->init(plugin)) {
            std::cerr << "CLAP plugin create/init failed\n";
            cleanup_entry();
            return 1;
        }

        auto destroy_plugin = [&]() {
            if (plugin != nullptr)
                plugin->destroy(plugin);
            cleanup_entry();
        };

        auto* gui = static_cast<const clap_plugin_gui_t*>(
            plugin->get_extension(plugin, CLAP_EXT_GUI));
        if (gui == nullptr) {
            std::cerr << "CLAP GUI extension unavailable\n";
            destroy_plugin();
            return 1;
        }
        if (!gui->is_api_supported(plugin, CLAP_WINDOW_API_COCOA, false)) {
            std::cerr << "CLAP Cocoa GUI API unsupported\n";
            destroy_plugin();
            return 1;
        }
        if (!gui->create(plugin, CLAP_WINDOW_API_COCOA, false)) {
            std::cerr << "CLAP gui.create failed\n";
            destroy_plugin();
            return 1;
        }

        const NSWindowStyleMask style = args.visible
            ? (NSWindowStyleMaskTitled | NSWindowStyleMaskClosable |
               NSWindowStyleMaskMiniaturizable)
            : NSWindowStyleMaskBorderless;
        NSWindow* window =
            [[NSWindow alloc] initWithContentRect:NSMakeRect(0, 0, args.width, args.height)
                                        styleMask:style
                                          backing:NSBackingStoreBuffered
                                            defer:NO];
        if (!window || !window.contentView) {
            std::cerr << "failed to create hidden Cocoa window\n";
            gui->destroy(plugin);
            destroy_plugin();
            return 1;
        }
        if (args.visible) {
            [window setTitle:@"Pulp ELYSIUM RUIF Rust CLAP"];
            [window center];
            [window makeKeyAndOrderFront:nil];
            [NSApp activateIgnoringOtherApps:YES];
        }

        clap_window_t cw{};
        cw.api = CLAP_WINDOW_API_COCOA;
        cw.cocoa = (__bridge void*)window.contentView;
        if (!gui->set_parent(plugin, &cw) || !gui->set_size(plugin, args.width, args.height)) {
            std::cerr << "CLAP gui attach/resize failed\n";
            [window close];
            gui->destroy(plugin);
            destroy_plugin();
            return 1;
        }

        pump_run_loop(10);

        auto* clap_inst =
            static_cast<pulp::format::clap_adapter::PulpClapPlugin*>(plugin->plugin_data);
        if (clap_inst == nullptr || clap_inst->editor_host == nullptr) {
            std::cerr << "Pulp CLAP editor host unavailable\n";
            [window close];
            gui->destroy(plugin);
            destroy_plugin();
            return 1;
        }

        if (!clap_inst->editor_host->is_gpu_backed()) {
            std::cerr << "Pulp CLAP editor host is not GPU-backed\n";
            [window close];
            gui->destroy(plugin);
            destroy_plugin();
            return 1;
        }

        const auto png = clap_inst->editor_host->capture_back_buffer_png();
        if (!looks_like_png(png)) {
            std::cerr << "GPU back-buffer capture did not return a PNG; bytes="
                      << png.size() << "\n";
            [window close];
            gui->destroy(plugin);
            destroy_plugin();
            return 1;
        }
        write_file(args.output_png, png);
        std::cout << "captured " << png.size() << " bytes from GPU CLAP editor: "
                  << args.output_png << "\n";

        if (args.hold_seconds > 0) {
            std::cout << "holding CLAP editor window for " << args.hold_seconds
                      << " seconds\n";
            const auto deadline = std::chrono::steady_clock::now()
                + std::chrono::seconds(args.hold_seconds);
            while (std::chrono::steady_clock::now() < deadline && [window isVisible]) {
                pump_run_loop(1);
            }
        }

        [window close];
        gui->destroy(plugin);
        destroy_plugin();
    }

    return 0;
}
