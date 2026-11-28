#include <pulp/vellum_renderer3d/renderer3d.hpp>

#include <pulp/scene/gltf_loader.hpp>

#include <cstdint>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string>

namespace {

struct Options {
    std::string scene = "hardcoded";
    std::filesystem::path fixture;
    std::filesystem::path output_png;
    std::string adapter_scope = "unscoped";
    std::string adapter_backend = "default";
    uint32_t width = 128;
    uint32_t height = 128;
    bool force_fallback_adapter = false;
    bool require_final_software_adapter = false;
};

void print_usage(const char* argv0) {
    std::cerr << "usage: " << argv0 << " [--scene hardcoded|boxtextured] "
              << "[--fixture path] [--width n] [--height n] "
              << "[--output-png path] "
              << "[--adapter-scope scope] [--adapter-backend default|null] "
              << "[--force-fallback-adapter] "
              << "[--require-final-software-adapter]\n";
}

bool parse_u32(const std::string& text, uint32_t& out) {
    try {
        size_t parsed = 0;
        const auto value = std::stoul(text, &parsed, 10);
        if (parsed != text.size() || value == 0 || value > static_cast<unsigned long>(UINT32_MAX)) {
            return false;
        }
        out = static_cast<uint32_t>(value);
        return true;
    } catch (...) {
        return false;
    }
}

bool parse_args(int argc, char** argv, Options& options) {
    for (int i = 1; i < argc; ++i) {
        const std::string arg = argv[i];
        auto require_value = [&](std::string& out) -> bool {
            if (i + 1 >= argc) {
                return false;
            }
            out = argv[++i];
            return true;
        };

        if (arg == "--scene") {
            if (!require_value(options.scene)) {
                return false;
            }
        } else if (arg == "--fixture") {
            std::string value;
            if (!require_value(value)) {
                return false;
            }
            options.fixture = value;
        } else if (arg == "--adapter-scope") {
            if (!require_value(options.adapter_scope)) {
                return false;
            }
        } else if (arg == "--output-png") {
            std::string value;
            if (!require_value(value)) {
                return false;
            }
            options.output_png = value;
        } else if (arg == "--adapter-backend") {
            if (!require_value(options.adapter_backend)) {
                return false;
            }
        } else if (arg == "--width") {
            std::string value;
            if (!require_value(value) || !parse_u32(value, options.width)) {
                return false;
            }
        } else if (arg == "--height") {
            std::string value;
            if (!require_value(value) || !parse_u32(value, options.height)) {
                return false;
            }
        } else if (arg == "--force-fallback-adapter") {
            options.force_fallback_adapter = true;
        } else if (arg == "--require-final-software-adapter") {
            options.require_final_software_adapter = true;
        } else if (arg == "--help" || arg == "-h") {
            print_usage(argv[0]);
            std::exit(0);
        } else {
            return false;
        }
    }

    return (options.scene == "hardcoded" || options.scene == "boxtextured") &&
           (options.adapter_backend == "default" || options.adapter_backend == "null");
}

bool is_pixel_producing_backend(const pulp::vellum_renderer3d::HardcodedCubeRenderResult& result) {
    return result.success && result.readback_completed && result.non_transparent_pixel_count > 0 &&
           result.adapter_backend_type != "Null";
}

bool is_final_software_eligible(const Options& options,
                                const pulp::vellum_renderer3d::HardcodedCubeRenderResult& result) {
    return is_pixel_producing_backend(result) && options.adapter_scope.rfind("software_", 0) == 0;
}

void print_bool(const char* key, bool value) {
    std::cout << key << "=" << (value ? "true" : "false") << "\n";
}

void print_result(const Options& options,
                  const pulp::vellum_renderer3d::HardcodedCubeRenderResult& result) {
    std::cout << "scene=" << options.scene << "\n";
    std::cout << "width=" << result.width << "\n";
    std::cout << "height=" << result.height << "\n";
    std::cout << "success=" << (result.success ? "true" : "false") << "\n";
    std::cout << "gpu_available=" << (result.gpu_available ? "true" : "false") << "\n";
    std::cout << "adapter_info_available=" << (result.adapter_info_available ? "true" : "false")
              << "\n";
    std::cout << "adapter_backend_type=" << result.adapter_backend_type << "\n";
    std::cout << "adapter_backend=" << result.adapter_backend << "\n";
    std::cout << "adapter_name=" << result.adapter_name << "\n";
    std::cout << "adapter_vendor=" << result.adapter_vendor << "\n";
    std::cout << "adapter_architecture=" << result.adapter_architecture << "\n";
    std::cout << "adapter_scope=" << options.adapter_scope << "\n";
    std::cout << "adapter_backend_preference=" << options.adapter_backend << "\n";
    print_bool("fallback_adapter_requested", result.fallback_adapter_requested);
    print_bool("null_backend_requested", result.null_backend_requested);
    print_bool("scene_data_consumed", result.scene_data_consumed);
    print_bool("depth_target_allocated", result.depth_target_allocated);
    print_bool("color_target_allocated", result.color_target_allocated);
    print_bool("vertex_buffer_uploaded", result.vertex_buffer_uploaded);
    print_bool("index_buffer_uploaded", result.index_buffer_uploaded);
    print_bool("uniform_buffer_uploaded", result.uniform_buffer_uploaded);
    print_bool("texture_uploaded", result.texture_uploaded);
    print_bool("texture_decoded", result.texture_decoded);
    print_bool("fallback_texture_used", result.fallback_texture_used);
    print_bool("base_color_texture_srgb_applied", result.base_color_texture_srgb_applied);
    print_bool("texture_sampler_applied", result.texture_sampler_applied);
    print_bool("texture_sampler_clamp_s", result.texture_sampler_clamp_s);
    print_bool("texture_sampler_clamp_t", result.texture_sampler_clamp_t);
    print_bool("texture_sampler_linear", result.texture_sampler_linear);
    print_bool("texture_mipmap_filter_downgraded", result.texture_mipmap_filter_downgraded);
    print_bool("base_color_transform_applied", result.base_color_transform_applied);
    print_bool("base_color_texcoord1_used", result.base_color_texcoord1_used);
    print_bool("base_color_factor_applied", result.base_color_factor_applied);
    print_bool("unlit_material_applied", result.unlit_material_applied);
    print_bool("alpha_mask_applied", result.alpha_mask_applied);
    print_bool("alpha_blend_applied", result.alpha_blend_applied);
    print_bool("alpha_blend_depth_write_disabled", result.alpha_blend_depth_write_disabled);
    print_bool("alpha_blend_sorted", result.alpha_blend_sorted);
    print_bool("vertex_color_applied", result.vertex_color_applied);
    print_bool("geometry_normals_applied", result.geometry_normals_applied);
    print_bool("metallic_roughness_factor_applied", result.metallic_roughness_factor_applied);
    print_bool("metallic_roughness_texture_applied", result.metallic_roughness_texture_applied);
    print_bool("double_sided_material_applied", result.double_sided_material_applied);
    print_bool("emissive_factor_applied", result.emissive_factor_applied);
    print_bool("emissive_strength_applied", result.emissive_strength_applied);
    print_bool("emissive_texture_applied", result.emissive_texture_applied);
    print_bool("directional_light_applied", result.directional_light_applied);
    print_bool("directional_light_transform_applied", result.directional_light_transform_applied);
    print_bool("light_node_transform_deferred", result.light_node_transform_deferred);
    print_bool("point_light_applied", result.point_light_applied);
    print_bool("point_light_deferred", result.point_light_deferred);
    print_bool("spot_light_applied", result.spot_light_applied);
    print_bool("spot_light_deferred", result.spot_light_deferred);
    print_bool("punctual_light_range_applied", result.punctual_light_range_applied);
    print_bool("punctual_light_range_deferred", result.punctual_light_range_deferred);
    print_bool("spot_light_cone_deferred", result.spot_light_cone_deferred);
    print_bool("perspective_camera_applied", result.perspective_camera_applied);
    print_bool("orthographic_camera_applied", result.orthographic_camera_applied);
    print_bool("camera_node_translation_applied", result.camera_node_translation_applied);
    print_bool("camera_node_rotation_applied", result.camera_node_rotation_applied);
    print_bool("camera_aspect_ratio_applied", result.camera_aspect_ratio_applied);
    print_bool("camera_aspect_ratio_deferred", result.camera_aspect_ratio_deferred);
    print_bool("camera_depth_range_applied", result.camera_depth_range_applied);
    print_bool("camera_depth_range_deferred", result.camera_depth_range_deferred);
    print_bool("camera_node_transform_deferred", result.camera_node_transform_deferred);
    print_bool("transform_animation_initial_pose_applied",
               result.transform_animation_initial_pose_applied);
    print_bool("transform_animation_deferred", result.transform_animation_deferred);
    print_bool("tangent_attributes_available", result.tangent_attributes_available);
    print_bool("tangent_attributes_derived", result.tangent_attributes_derived);
    print_bool("normal_texture_applied", result.normal_texture_applied);
    print_bool("normal_scale_applied", result.normal_scale_applied);
    print_bool("metallic_roughness_texture_deferred", result.metallic_roughness_texture_deferred);
    print_bool("normal_texture_deferred", result.normal_texture_deferred);
    print_bool("normal_scale_deferred", result.normal_scale_deferred);
    print_bool("occlusion_texture_applied", result.occlusion_texture_applied);
    print_bool("occlusion_strength_applied", result.occlusion_strength_applied);
    print_bool("occlusion_texture_deferred", result.occlusion_texture_deferred);
    print_bool("occlusion_strength_deferred", result.occlusion_strength_deferred);
    print_bool("emissive_texture_deferred", result.emissive_texture_deferred);
    print_bool("non_base_color_texture_transform_applied",
               result.non_base_color_texture_transform_applied);
    print_bool("non_base_color_texcoord1_used", result.non_base_color_texcoord1_used);
    print_bool("non_base_color_texture_transform_deferred",
               result.non_base_color_texture_transform_deferred);
    print_bool("non_base_color_texcoord1_deferred", result.non_base_color_texcoord1_deferred);
    print_bool("advanced_material_extension_deferred", result.advanced_material_extension_deferred);
    print_bool("skinning_deferred", result.skinning_deferred);
    print_bool("morph_target_deferred", result.morph_target_deferred);
    print_bool("gpu_instancing_deferred", result.gpu_instancing_deferred);
    print_bool("command_submitted", result.command_submitted);
    print_bool("readback_completed", result.readback_completed);
    std::cout << "primitive_count=" << result.primitive_count << "\n";
    std::cout << "pipeline_cache_entry_count=" << result.pipeline_cache_entry_count << "\n";
    std::cout << "pipeline_cache_hit_count=" << result.pipeline_cache_hit_count << "\n";
    std::cout << "distinct_color_count=" << result.distinct_color_count << "\n";
    std::cout << "non_transparent_pixel_count=" << result.non_transparent_pixel_count << "\n";
    std::cout << "rgba_fingerprint=" << pulp::vellum_renderer3d::rgba_fingerprint(result) << "\n";
    std::cout << "pixel_output_produced=" << (is_pixel_producing_backend(result) ? "true" : "false")
              << "\n";
    std::cout << "final_software_golden_eligible="
              << (is_final_software_eligible(options, result) ? "true" : "false") << "\n";
    if (!result.error.empty()) {
        std::cout << "error=" << result.error << "\n";
    }
}

bool write_output_png(const Options& options,
                      const pulp::vellum_renderer3d::HardcodedCubeRenderResult& result) {
    if (options.output_png.empty()) {
        return true;
    }
    if (result.png.empty()) {
        std::cerr << "pulp-vellum-renderer3d-probe: PNG output requested but render "
                  << "produced no PNG bytes\n";
        return false;
    }
    std::ofstream png(options.output_png, std::ios::binary);
    if (!png) {
        std::cerr << "pulp-vellum-renderer3d-probe: failed to open PNG output path: "
                  << options.output_png << "\n";
        return false;
    }
    png.write(reinterpret_cast<const char*>(result.png.data()),
              static_cast<std::streamsize>(result.png.size()));
    if (!png.good()) {
        std::cerr << "pulp-vellum-renderer3d-probe: failed to write PNG output path: "
                  << options.output_png << "\n";
        return false;
    }
    return true;
}

} // namespace

int main(int argc, char** argv) {
    Options options;
    if (!parse_args(argc, argv, options)) {
        print_usage(argv[0]);
        return 64;
    }

    pulp::vellum_renderer3d::HardcodedCubeRenderResult result;
    if (options.scene == "hardcoded") {
        pulp::vellum_renderer3d::HardcodedCubeRenderConfig config;
        config.width = options.width;
        config.height = options.height;
        config.force_fallback_adapter = options.force_fallback_adapter;
        if (options.adapter_backend == "null") {
            config.backend_preference =
                pulp::vellum_renderer3d::Renderer3DAdapterBackendPreference::null_backend;
        }
        result = pulp::vellum_renderer3d::Renderer3D::render_hardcoded_textured_cube(config);
    } else {
        if (options.fixture.empty()) {
            std::cerr << "pulp-vellum-renderer3d-probe: --fixture is required for "
                      << "--scene boxtextured\n";
            return 64;
        }
        const auto loaded = pulp::scene::load_gltf_scene(options.fixture);
        if (!loaded.success) {
            std::cout << "scene=" << options.scene << "\n";
            std::cout << "success=false\n";
            std::cout << "error=" << loaded.error << "\n";
            return 2;
        }
        pulp::vellum_renderer3d::SceneDataRenderConfig config;
        config.width = options.width;
        config.height = options.height;
        config.force_fallback_adapter = options.force_fallback_adapter;
        if (options.adapter_backend == "null") {
            config.backend_preference =
                pulp::vellum_renderer3d::Renderer3DAdapterBackendPreference::null_backend;
        }
        result = pulp::vellum_renderer3d::Renderer3D::render_scene_data(loaded.scene, config);
    }

    print_result(options, result);
    if (!write_output_png(options, result)) {
        return 2;
    }
    if (options.require_final_software_adapter && !is_final_software_eligible(options, result)) {
        return 3;
    }
    return is_pixel_producing_backend(result) ? 0 : 2;
}
