#include "elysium_rust_provider_bridge.hpp"

#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/window_host.hpp>

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#if defined(__unix__)
#include <unistd.h>
#endif

namespace {

std::filesystem::path asset_dir() {
    if (const char* env = std::getenv("PULP_ELYSIUM_RUIF_ASSET_DIR")) {
        if (*env != '\0')
            return env;
    }
    return PULP_ELYSIUM_RUIF_ASSET_DIR;
}

bool set_working_directory(const std::filesystem::path& dir) {
#if defined(__unix__)
    return ::chdir(dir.c_str()) == 0;
#else
    std::error_code ec;
    std::filesystem::current_path(dir, ec);
    return !ec;
#endif
}

bool write_png(const std::filesystem::path& path, const std::vector<uint8_t>& png) {
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open())
        return false;
    out.write(reinterpret_cast<const char*>(png.data()), static_cast<std::streamsize>(png.size()));
    return out.good();
}

bool write_text(const std::filesystem::path& path, std::string_view text) {
    if (path.has_parent_path())
        std::filesystem::create_directories(path.parent_path());
    std::ofstream out(path, std::ios::binary);
    if (!out.is_open())
        return false;
    out << text;
    return out.good();
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path screenshot_path;
    std::filesystem::path layout_path;
    for (int i = 1; i < argc; ++i) {
        const std::string arg(argv[i]);
        constexpr std::string_view screenshot_prefix = "--screenshot=";
        constexpr std::string_view layout_prefix = "--layout=";
        if (arg.rfind(screenshot_prefix, 0) == 0) {
            screenshot_path = arg.substr(screenshot_prefix.size());
        } else if (arg.rfind(layout_prefix, 0) == 0) {
            layout_path = arg.substr(layout_prefix.size());
        }
    }
    if (!screenshot_path.empty())
        screenshot_path = std::filesystem::absolute(screenshot_path);
    if (!layout_path.empty())
        layout_path = std::filesystem::absolute(layout_path);

    const auto assets = asset_dir();
    if (!std::filesystem::exists(assets / "assets")) {
        std::cerr << "ELYSIUM assets not found at " << (assets / "assets") << "\n";
        return 1;
    }
    if (!set_working_directory(assets)) {
        std::cerr << "failed to set ELYSIUM asset working directory: " << assets << "\n";
        return 1;
    }

    std::string failure_reason;
    auto root = pulp::examples::build_elysium_rust_provider_ui(&failure_reason);
    if (!root) {
        std::cerr << failure_reason << "\n";
        return 1;
    }

    root->set_bounds({0.0f, 0.0f, 1000.0f, 600.0f});
    root->layout_children();

    if (!layout_path.empty()) {
        const auto layout = pulp::view::dump_layout_tree(
            *root,
            {.surface = "ruif-standalone",
             .fixture = "elysium-rust-provider",
             .viewport_width = 1000.0f,
             .viewport_height = 600.0f});
        if (!write_text(layout_path, layout)) {
            std::cerr << "failed to write ELYSIUM Rust-provider layout snapshot: "
                      << layout_path << "\n";
            return 1;
        }
    }

    pulp::view::WindowOptions options;
    options.title = "Pulp ELYSIUM RUIF C++ Baseline";
    options.width = 1000.0f;
    options.height = 600.0f;
    options.min_width = 667.0f;
    options.min_height = 400.0f;
    options.resizable = true;
    options.use_gpu = true;
    options.initially_hidden = !screenshot_path.empty();

    auto window = pulp::view::WindowHost::create(*root, options);
    if (!window) {
        std::cerr << "failed to create ELYSIUM Rust-provider GPU window host\n";
        return 1;
    }
    window->set_design_viewport(1000.0f, 600.0f);
    window->set_fixed_aspect_ratio(1000.0f / 600.0f);
    window->set_close_callback([] {});

    if (!screenshot_path.empty()) {
        auto png = window->capture_back_buffer_png();
        if (png.empty() || !write_png(screenshot_path, png)) {
            std::cerr << "failed to capture ELYSIUM Rust-provider GPU screenshot: "
                      << screenshot_path << "\n";
            return 1;
        }
        return 0;
    }

    window->run_event_loop();
    return 0;
}
