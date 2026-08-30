#include <pulp/scene/gltf_loader.hpp>
#include <pulp/scene/render_packet.hpp>
#include <pulp/scene/scene_stats.hpp>

#ifdef PULP_SCENE3D_INSPECT_NATIVE_DRACO
#include <pulp/render/draco_decoder.hpp>
#include <pulp/scene/draco_scene_adapter.hpp>
#endif

#include <cstddef>
#include <filesystem>
#include <iomanip>
#include <iostream>
#include <sstream>
#include <string_view>

namespace {

void print_usage(const char* argv0) {
    std::cerr << "Usage: " << argv0
#ifdef PULP_SCENE3D_INSPECT_NATIVE_DRACO
              << " [--native-draco] [--render-packet] <scene.glb|scene.gltf>\n";
#else
              << " [--render-packet] <scene.glb|scene.gltf>\n";
#endif
}

std::string join_feature_names(const pulp::scene::MaterialKey& key) {
    const auto names = pulp::scene::material_feature_names(key);
    if (names.empty()) {
        return "none";
    }
    std::ostringstream out;
    for (size_t i = 0; i < names.size(); ++i) {
        if (i > 0) {
            out << ",";
        }
        out << names[i];
    }
    return out.str();
}

std::string format_mat4(const pulp::scene::Mat4& transform) {
    std::ostringstream out;
    out << std::setprecision(9);
    for (size_t i = 0; i < 16; ++i) {
        if (i > 0) {
            out << ",";
        }
        out << transform.m[i];
    }
    return out.str();
}

void print_render_packet(const pulp::scene::SceneData& scene) {
    const auto packet = pulp::scene::build_render_packet(scene);
    std::cout << "render_packet transformed_nodes="
              << packet.transformed_nodes.size()
              << " primitives=" << packet.primitives.size()
              << " diagnostics=" << packet.diagnostics.size()
              << " has_errors=" << (packet.has_errors() ? "true" : "false")
              << "\n";
    for (const auto& diagnostic : packet.diagnostics) {
        std::cout << "diagnostic "
                  << pulp::scene::diagnostic_severity_name(diagnostic.severity)
                  << " " << diagnostic.code;
        if (!diagnostic.message.empty()) {
            std::cout << " " << diagnostic.message;
        }
        std::cout << "\n";
    }
    for (const auto& primitive : packet.primitives) {
        std::cout << "primitive node=" << primitive.node
                  << " mesh=" << primitive.mesh
                  << " primitive=" << primitive.primitive
                  << " material=" << primitive.material_key.material
                  << " feature_mask=" << primitive.material_key.feature_mask
                  << " world_transform="
                  << format_mat4(primitive.world_transform)
                  << " features="
                  << join_feature_names(primitive.material_key) << "\n";
    }
}

} // namespace

int main(int argc, char** argv) {
    if (argc < 2 || std::string_view(argv[1]) == "--help" ||
        std::string_view(argv[1]) == "-h") {
        print_usage(argv[0]);
        return argc == 2 ? 0 : 64;
    }

    bool dump_render_packet = false;
    bool native_draco = false;
    int path_arg = 0;
    for (int i = 1; i < argc; ++i) {
        const std::string_view arg(argv[i]);
        if (arg == "--render-packet") {
            dump_render_packet = true;
            continue;
        }
        if (arg == "--native-draco") {
#ifdef PULP_SCENE3D_INSPECT_NATIVE_DRACO
            native_draco = true;
            continue;
#else
            print_usage(argv[0]);
            return 64;
#endif
        }
        if (arg.starts_with("-")) {
            print_usage(argv[0]);
            return 64;
        }
        if (path_arg != 0) {
            print_usage(argv[0]);
            return 64;
        }
        path_arg = i;
    }
    if (path_arg == 0) {
        print_usage(argv[0]);
        return 64;
    }

    const auto path = std::filesystem::path(argv[path_arg]);
    pulp::scene::LoadOptions options;
#ifdef PULP_SCENE3D_INSPECT_NATIVE_DRACO
    if (native_draco) {
        options.draco_decode = pulp::scene::make_scene_draco_decode_callback();
        std::cout << "native_draco_decoder_available="
                  << (pulp::render::draco_decoder_available() ? "true" : "false")
                  << "\n";
    }
#else
    (void)native_draco;
#endif
    const auto result = pulp::scene::load_gltf_scene(path, options);
    if (!result.success) {
        std::cerr << "pulp-scene3d-inspect: " << result.error << "\n";
        for (const auto& diagnostic : result.scene.diagnostics) {
            std::cerr << pulp::scene::diagnostic_severity_name(diagnostic.severity)
                      << ": " << diagnostic.code << ": "
                      << diagnostic.message << "\n";
        }
        return 2;
    }

    const auto stats = pulp::scene::summarize_scene_data(result.scene);
    std::cout << pulp::scene::scene_stats_to_text(stats) << "\n";
    if (dump_render_packet) {
        print_render_packet(result.scene);
    }
    return pulp::scene::has_error_diagnostics(result.scene.diagnostics) ? 1 : 0;
}
