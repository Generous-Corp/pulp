#include <pulp/view/design_import.hpp>
#include <pulp/view/layout_snapshot.hpp>
#include <pulp/view/screenshot.hpp>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <sstream>
#include <string>

namespace {

bool parse_positive(const char* value, float& result) {
    try {
        result = std::stof(value);
        return result > 0.0f;
    } catch (...) {
        return false;
    }
}

std::string read_text(const std::filesystem::path& path) {
    std::ifstream input(path, std::ios::binary);
    if (!input) return {};
    return {std::istreambuf_iterator<char>(input),
            std::istreambuf_iterator<char>()};
}

bool write_text(const std::filesystem::path& path, const std::string& text) {
    std::error_code error;
    if (!path.parent_path().empty())
        std::filesystem::create_directories(path.parent_path(), error);
    if (error) return false;
    std::ofstream output(path, std::ios::binary | std::ios::trunc);
    output << text << '\n';
    return static_cast<bool>(output);
}

void usage() {
    std::cerr
        << "Usage: pulp-design-ir-observe --input <design.ir.json> "
           "--render <png> --layout <json> --width <px> --height <px> "
           "[--scale <factor>]\n";
}

}  // namespace

int main(int argc, char** argv) {
    std::filesystem::path input_path;
    std::filesystem::path render_path;
    std::filesystem::path layout_path;
    float width = 0.0f;
    float height = 0.0f;
    float scale = 2.0f;
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        if (i + 1 >= argc) {
            usage();
            return 2;
        }
        const char* value = argv[++i];
        if (arg == "--input") input_path = value;
        else if (arg == "--render") render_path = value;
        else if (arg == "--layout") layout_path = value;
        else if (arg == "--width" && parse_positive(value, width)) {}
        else if (arg == "--height" && parse_positive(value, height)) {}
        else if (arg == "--scale" && parse_positive(value, scale)) {}
        else {
            usage();
            return 2;
        }
    }
    if (input_path.empty() || render_path.empty() || layout_path.empty() ||
        width <= 0.0f || height <= 0.0f) {
        usage();
        return 2;
    }
    const auto serialized = read_text(input_path);
    if (serialized.empty()) {
        std::cerr << "Error: could not read DesignIR input\n";
        return 1;
    }
    auto ir = pulp::view::parse_design_ir_json(serialized);
    auto root = pulp::view::build_native_view_tree(ir, ir.asset_manifest);
    if (!root) {
        std::cerr << "Error: could not materialize DesignIR\n";
        return 1;
    }
    root->set_bounds({0.0f, 0.0f, width, height});
    if (!pulp::view::render_to_file(
            *root,
            static_cast<std::uint32_t>(width),
            static_cast<std::uint32_t>(height),
            render_path.string(),
            scale,
            pulp::view::ScreenshotBackend::skia)) {
        std::cerr << "Error: could not render DesignIR through Skia\n";
        return 1;
    }
    const auto layout = pulp::view::dump_layout_tree(
        *root,
        {.surface = "design-ir-observer",
         .fixture = input_path.filename().string(),
         .viewport_width = width,
         .viewport_height = height});
    if (!write_text(layout_path, layout)) {
        std::cerr << "Error: could not write layout observation\n";
        return 1;
    }
    return 0;
}
