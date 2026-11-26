#pragma once

#include "suite_shared.hpp"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

namespace {

bool has_native_runtime_gap(const pulp::scene::BakePreflightReport& report,
                            const std::string& feature) {
    for (const auto& gap : report.native_runtime_gaps) {
        if (gap.feature == feature) {
            return true;
        }
    }
    return false;
}

double average_image_luma(const std::vector<uint8_t>& rgba, uint32_t width, uint32_t height) {
    if (rgba.size() < static_cast<size_t>(width) * height * 4u || width == 0 || height == 0) {
        return 0.0;
    }

    double sum = 0.0;
    for (uint32_t y = 0; y < height; ++y) {
        for (uint32_t x = 0; x < width; ++x) {
            const size_t offset = (static_cast<size_t>(y) * width + x) * 4u;
            sum += 0.2126 * static_cast<double>(rgba[offset + 0]) +
                   0.7152 * static_cast<double>(rgba[offset + 1]) +
                   0.0722 * static_cast<double>(rgba[offset + 2]);
        }
    }
    return sum / static_cast<double>(width * height);
}

pulp::scene::SceneData make_transform_animation_render_scene(float initial_x = 0.35f) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "animated-transform-material";
    material.base_color_factor[0] = 0.85f;
    material.base_color_factor[1] = 0.85f;
    material.base_color_factor[2] = 0.85f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "animated-transform-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "animated-node";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    pulp::scene::NodeData anchor_node;
    anchor_node.name = "static-anchor-node";
    anchor_node.mesh = 0;
    scene.nodes.push_back(std::move(anchor_node));
    scene.root_nodes.push_back(1);

    pulp::scene::AnimationSamplerData sampler;
    sampler.input_times = {0.0f, 1.0f};
    sampler.output_values = {
        initial_x, 0.0f, 0.0f, 0.35f, 0.0f, 0.0f,
    };
    sampler.output_components = 3;

    pulp::scene::AnimationData animation;
    animation.name = "translation-only";
    animation.samplers.push_back(std::move(sampler));
    animation.channels.push_back(pulp::scene::AnimationChannelData{
        0,
        0,
        pulp::scene::AnimationChannelData::Path::translation,
    });
    scene.animations.push_back(std::move(animation));

    return scene;
}

pulp::scene::SceneData make_rotation_animation_render_scene() {
    auto scene = make_transform_animation_render_scene(0.0f);
    scene.animations.clear();

    pulp::scene::AnimationSamplerData sampler;
    sampler.input_times = {0.0f, 1.0f};
    sampler.output_components = 4;
    sampler.interpolation = pulp::scene::AnimationSamplerData::Interpolation::cubic_spline;
    sampler.output_values = {
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.7071068f, 0.7071068f, 0.0f, 0.0f, 0.0f, 0.0f,
        0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f,       1.0f,       0.0f, 0.0f, 0.0f, 0.0f,
    };

    pulp::scene::AnimationData animation;
    animation.name = "rotation-cubic-initial-pose";
    animation.samplers.push_back(std::move(sampler));
    animation.channels.push_back(pulp::scene::AnimationChannelData{
        0,
        0,
        pulp::scene::AnimationChannelData::Path::rotation,
    });
    scene.animations.push_back(std::move(animation));
    return scene;
}

pulp::scene::SceneData make_scale_animation_render_scene() {
    auto scene = make_transform_animation_render_scene(0.0f);
    scene.animations.clear();

    pulp::scene::AnimationSamplerData sampler;
    sampler.input_times = {0.0f, 1.0f};
    sampler.output_values = {
        1.45f, 0.7f, 1.0f, 1.0f, 1.0f, 1.0f,
    };
    sampler.output_components = 3;

    pulp::scene::AnimationData animation;
    animation.name = "scale-initial-pose";
    animation.samplers.push_back(std::move(sampler));
    animation.channels.push_back(pulp::scene::AnimationChannelData{
        0,
        0,
        pulp::scene::AnimationChannelData::Path::scale,
    });
    scene.animations.push_back(std::move(animation));
    return scene;
}

pulp::scene::SceneData make_unsupported_feature_deferred_render_scene() {
    auto scene = make_perspective_camera_render_scene(true);
    scene.unsupported_features.push_back(pulp::scene::UnsupportedFeatureData{
        "Skinning",
        "Node references a glTF skin, but native skinning is not implemented in this slice.",
        "SkinnedNode",
    });
    scene.unsupported_features.push_back(pulp::scene::UnsupportedFeatureData{
        "MorphTargets",
        "Primitive morph targets are present, but native morph target evaluation is not "
        "implemented in this slice.",
        "MorphedMesh",
    });
    scene.unsupported_features.push_back(pulp::scene::UnsupportedFeatureData{
        "GpuInstancing",
        "EXT_mesh_gpu_instancing attributes are present, but native instanced rendering is not "
        "implemented in this slice.",
        "InstancedNode",
    });
    return scene;
}

pulp::scene::SceneData make_advanced_material_extension_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "physical-extension-material";
    material.base_color_factor[0] = 0.75f;
    material.base_color_factor[1] = 0.82f;
    material.base_color_factor[2] = 0.92f;
    material.double_sided = true;
    material.advanced_material_extensions.push_back("KHR_materials_clearcoat");
    material.advanced_material_extensions.push_back("KHR_materials_transmission");
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "physical-extension-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "physical-extension-node";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_unlit_render_scene(bool unlit) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = unlit ? "unlit-material" : "lit-material";
    material.unlit = unlit;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "unlit-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

std::vector<uint8_t> make_blue_pixel_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
        0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
        0x9c, 0x63, 0x60, 0x60, 0xf8, 0xff, 0x1f, 0x00, 0x03, 0x02, 0x01, 0xff, 0xe6, 0x77,
        0x0b, 0xae, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

std::vector<uint8_t> make_black_pixel_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
        0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
        0x9c, 0x63, 0x60, 0x60, 0x60, 0xf8, 0x0f, 0x00, 0x01, 0x04, 0x01, 0x00, 0x5f, 0xe5,
        0xc3, 0x4b, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

std::vector<uint8_t> make_black_blue_columns_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72,
        0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x12, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60,
        0x60, 0x60, 0xf8, 0x0f, 0x44, 0x40, 0x0c, 0x65, 0x00, 0x00, 0x2a, 0xe7, 0x05, 0xfb, 0x23,
        0x1f, 0xcb, 0xf2, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

std::vector<uint8_t> make_black_white_columns_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44,
        0x52, 0x00, 0x00, 0x00, 0x02, 0x00, 0x00, 0x00, 0x02, 0x08, 0x06, 0x00, 0x00, 0x00, 0x72,
        0xb6, 0x0d, 0x24, 0x00, 0x00, 0x00, 0x11, 0x49, 0x44, 0x41, 0x54, 0x78, 0xda, 0x63, 0x60,
        0x60, 0x60, 0xf8, 0x0f, 0x02, 0x0c, 0x30, 0x06, 0x00, 0x4a, 0xc7, 0x09, 0xf7, 0x3d, 0x02,
        0x94, 0x2f, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

std::vector<uint8_t> make_reverse_z_normal_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
        0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
        0x9c, 0x63, 0x68, 0x68, 0x60, 0xf8, 0x0f, 0x00, 0x04, 0x84, 0x02, 0x00, 0x0a, 0x2c,
        0x99, 0xd6, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

std::vector<uint8_t> make_tilted_normal_png() {
    return {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
        0x44, 0x52, 0x00, 0x00, 0x00, 0x01, 0x00, 0x00, 0x00, 0x01, 0x08, 0x06, 0x00, 0x00,
        0x00, 0x1f, 0x15, 0xc4, 0x89, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x44, 0x41, 0x54, 0x78,
        0xda, 0x63, 0xf8, 0x9f, 0x70, 0xe0, 0x3f, 0x00, 0x07, 0xa0, 0x03, 0x1f, 0x73, 0x5c,
        0x34, 0x35, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
    };
}

pulp::scene::SceneData make_alpha_mask_render_scene(bool masked) {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData texture;
    texture.name = "alpha-checker";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_alpha_checker_png();
    scene.textures.push_back(std::move(texture));

    pulp::scene::MaterialData material;
    material.name = masked ? "masked-material" : "opaque-material";
    material.base_color_texture = 0;
    if (masked) {
        material.alpha_mode = pulp::scene::MaterialData::AlphaMode::mask;
        material.alpha_cutoff = 0.5f;
    }
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "alpha-mask-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_vertex_color_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "vertex-color-material";
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.color0 = {
        1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "vertex-color-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_back_facing_render_scene(bool double_sided) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = double_sided ? "double-sided-material" : "single-sided-material";
    material.double_sided = double_sided;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 1.0f, -1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 0.5f, 1.0f, 1.0f, 0.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "back-facing-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_alpha_blend_render_scene(bool blended) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = blended ? "blend-material" : "opaque-material";
    material.base_color_factor[0] = 1.0f;
    material.base_color_factor[1] = 1.0f;
    material.base_color_factor[2] = 1.0f;
    material.base_color_factor[3] = blended ? 0.35f : 1.0f;
    if (blended) {
        material.alpha_mode = pulp::scene::MaterialData::AlphaMode::blend;
    }
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "alpha-blend-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_alpha_blend_over_opaque_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData red_blend;
    red_blend.name = "red-blend-front";
    red_blend.base_color_factor[0] = 1.0f;
    red_blend.base_color_factor[1] = 0.05f;
    red_blend.base_color_factor[2] = 0.05f;
    red_blend.base_color_factor[3] = 0.45f;
    red_blend.alpha_mode = pulp::scene::MaterialData::AlphaMode::blend;
    red_blend.double_sided = true;
    scene.materials.push_back(red_blend);

    pulp::scene::MaterialData green_opaque;
    green_opaque.name = "green-opaque-back";
    green_opaque.base_color_factor[0] = 0.05f;
    green_opaque.base_color_factor[1] = 1.0f;
    green_opaque.base_color_factor[2] = 0.05f;
    green_opaque.double_sided = true;
    scene.materials.push_back(green_opaque);

    pulp::scene::PrimitiveData blended_front;
    blended_front.positions = {
        -1.0f, -1.0f, 0.35f, 1.0f, -1.0f, 0.35f, 0.0f, 1.0f, 0.35f,
    };
    blended_front.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    blended_front.indices = {0, 1, 2};
    blended_front.material = 0;

    pulp::scene::PrimitiveData opaque_back;
    opaque_back.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    opaque_back.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    opaque_back.indices = {0, 1, 2};
    opaque_back.material = 1;

    pulp::scene::MeshData mesh;
    mesh.name = "alpha-blend-over-opaque";
    mesh.primitives.push_back(std::move(blended_front));
    mesh.primitives.push_back(std::move(opaque_back));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_alpha_blend_sorted_layers_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData red_front;
    red_front.name = "red-blend-front";
    red_front.base_color_factor[0] = 1.0f;
    red_front.base_color_factor[1] = 0.05f;
    red_front.base_color_factor[2] = 0.05f;
    red_front.base_color_factor[3] = 0.55f;
    red_front.alpha_mode = pulp::scene::MaterialData::AlphaMode::blend;
    red_front.double_sided = true;
    scene.materials.push_back(red_front);

    pulp::scene::MaterialData blue_back;
    blue_back.name = "blue-blend-back";
    blue_back.base_color_factor[0] = 0.05f;
    blue_back.base_color_factor[1] = 0.05f;
    blue_back.base_color_factor[2] = 1.0f;
    blue_back.base_color_factor[3] = 0.55f;
    blue_back.alpha_mode = pulp::scene::MaterialData::AlphaMode::blend;
    blue_back.double_sided = true;
    scene.materials.push_back(blue_back);

    pulp::scene::PrimitiveData front_submitted_first;
    front_submitted_first.positions = {
        -1.0f, -1.0f, 0.35f, 1.0f, -1.0f, 0.35f, 0.0f, 1.0f, 0.35f,
    };
    front_submitted_first.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    front_submitted_first.indices = {0, 1, 2};
    front_submitted_first.material = 0;

    pulp::scene::PrimitiveData back_submitted_second;
    back_submitted_second.positions = {
        -1.0f, -1.0f, -0.35f, 1.0f, -1.0f, -0.35f, 0.0f, 1.0f, -0.35f,
    };
    back_submitted_second.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    back_submitted_second.indices = {0, 1, 2};
    back_submitted_second.material = 1;

    pulp::scene::MeshData mesh;
    mesh.name = "alpha-blend-sorted-layers";
    mesh.primitives.push_back(std::move(front_submitted_first));
    mesh.primitives.push_back(std::move(back_submitted_second));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_emissive_render_scene(bool emissive, float emissive_strength = 1.0f) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = emissive ? "emissive-material" : "plain-material";
    material.base_color_factor[0] = 0.1f;
    material.base_color_factor[1] = 0.1f;
    material.base_color_factor[2] = 0.1f;
    if (emissive) {
        material.emissive_factor[2] = 0.65f;
        material.emissive_strength = emissive_strength;
    }
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "emissive-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_emissive_texture_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData texture;
    texture.name = "blue-emissive";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_blue_pixel_png();
    scene.textures.push_back(std::move(texture));

    pulp::scene::MaterialData material;
    material.name = "emissive-texture-material";
    material.base_color_factor[0] = 0.05f;
    material.base_color_factor[1] = 0.05f;
    material.base_color_factor[2] = 0.05f;
    material.emissive_factor[0] = 1.0f;
    material.emissive_factor[1] = 1.0f;
    material.emissive_factor[2] = 1.0f;
    material.emissive_strength = 1.0f;
    material.emissive_texture = 0;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "emissive-texture-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_emissive_texture_texcoord1_transform_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData texture;
    texture.name = "black-blue-emissive";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_black_blue_columns_png();
    scene.textures.push_back(std::move(texture));

    pulp::scene::TextureSamplerData sampler;
    sampler.name = "nearest-clamp";
    sampler.mag_filter = pulp::scene::TextureSamplerData::Filter::nearest;
    sampler.min_filter = pulp::scene::TextureSamplerData::Filter::nearest;
    sampler.wrap_s = pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
    sampler.wrap_t = pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
    scene.texture_samplers.push_back(sampler);

    pulp::scene::MaterialData material;
    material.name = "emissive-texcoord1-transform-material";
    material.base_color_factor[0] = 0.02f;
    material.base_color_factor[1] = 0.02f;
    material.base_color_factor[2] = 0.02f;
    material.emissive_factor[0] = 1.0f;
    material.emissive_factor[1] = 1.0f;
    material.emissive_factor[2] = 1.0f;
    material.emissive_strength = 1.0f;
    material.emissive_texture = 0;
    material.emissive_sampler = 0;
    material.emissive_texcoord = 1;
    material.emissive_transform.enabled = true;
    material.emissive_transform.offset[0] = 0.5f;
    material.emissive_transform.offset[1] = 0.0f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.5f, 0.0f, 0.5f, 0.0f, 0.5f,
    };
    primitive.texcoord1 = {
        0.25f, 0.5f, 0.25f, 0.5f, 0.25f, 0.5f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "emissive-texcoord1-transform-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_metallic_roughness_factor_render_scene(float metallic,
                                                                   float roughness) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "metallic-roughness-factor-material";
    material.base_color_factor[0] = 0.8f;
    material.base_color_factor[1] = 0.8f;
    material.base_color_factor[2] = 0.8f;
    material.metallic_factor = metallic;
    material.roughness_factor = roughness;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "metallic-roughness-factor-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_metallic_roughness_texture_render_scene() {
    auto scene = make_metallic_roughness_factor_render_scene(1.0f, 1.0f);

    pulp::scene::TextureData texture;
    texture.name = "black-metallic-roughness";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_black_pixel_png();
    scene.textures.push_back(std::move(texture));

    scene.materials[0].metallic_roughness_texture = 0;
    return scene;
}

pulp::scene::SceneData make_metallic_roughness_texcoord1_transform_render_scene(bool transformed) {
    auto scene = make_metallic_roughness_factor_render_scene(1.0f, 1.0f);

    pulp::scene::TextureData texture;
    texture.name = "black-blue-metallic-roughness";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_black_blue_columns_png();
    scene.textures.push_back(std::move(texture));

    pulp::scene::TextureSamplerData sampler;
    sampler.name = "nearest-clamp";
    sampler.mag_filter = pulp::scene::TextureSamplerData::Filter::nearest;
    sampler.min_filter = pulp::scene::TextureSamplerData::Filter::nearest;
    sampler.wrap_s = pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
    sampler.wrap_t = pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
    scene.texture_samplers.push_back(sampler);

    scene.materials[0].metallic_roughness_texture = 0;
    scene.materials[0].metallic_roughness_sampler = 0;
    scene.materials[0].metallic_roughness_texcoord = 1;
    scene.materials[0].metallic_roughness_transform.enabled = true;
    scene.materials[0].metallic_roughness_transform.offset[0] = transformed ? 0.5f : 0.0f;

    auto& primitive = scene.meshes[0].primitives[0];
    primitive.texcoord0 = {
        0.0f, 0.5f, 0.0f, 0.5f, 0.0f, 0.5f,
    };
    primitive.texcoord1 = {
        0.25f, 0.5f, 0.25f, 0.5f, 0.25f, 0.5f,
    };

    return scene;
}

pulp::scene::SceneData make_occlusion_texture_render_scene(float occlusion_strength = 1.0f) {
    auto scene = make_metallic_roughness_factor_render_scene(0.0f, 0.1f);

    pulp::scene::TextureData texture;
    texture.name = "black-occlusion";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_black_pixel_png();
    scene.textures.push_back(std::move(texture));

    scene.materials[0].occlusion_texture = 0;
    scene.materials[0].occlusion_strength = occlusion_strength;
    return scene;
}

pulp::scene::SceneData make_occlusion_texcoord1_transform_render_scene(bool transformed) {
    auto scene = make_metallic_roughness_factor_render_scene(0.0f, 0.1f);

    pulp::scene::TextureData texture;
    texture.name = "black-white-occlusion";
    texture.mime_type = "image/png";
    texture.encoded_bytes = make_black_white_columns_png();
    scene.textures.push_back(std::move(texture));

    pulp::scene::TextureSamplerData sampler;
    sampler.name = "nearest-clamp";
    sampler.mag_filter = pulp::scene::TextureSamplerData::Filter::nearest;
    sampler.min_filter = pulp::scene::TextureSamplerData::Filter::nearest;
    sampler.wrap_s = pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
    sampler.wrap_t = pulp::scene::TextureSamplerData::Wrap::clamp_to_edge;
    scene.texture_samplers.push_back(sampler);

    scene.materials[0].occlusion_texture = 0;
    scene.materials[0].occlusion_sampler = 0;
    scene.materials[0].occlusion_texcoord = 1;
    scene.materials[0].occlusion_transform.enabled = true;
    scene.materials[0].occlusion_transform.offset[0] = transformed ? 0.5f : 0.0f;

    auto& primitive = scene.meshes[0].primitives[0];
    primitive.texcoord0 = {
        0.0f, 0.5f, 0.0f, 0.5f, 0.0f, 0.5f,
    };
    primitive.texcoord1 = {
        0.25f, 0.5f, 0.25f, 0.5f, 0.25f, 0.5f,
    };

    return scene;
}

pulp::scene::SceneData make_geometry_normal_render_scene(bool toward_light) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = toward_light ? "front-normal-material" : "back-normal-material";
    material.base_color_factor[0] = 0.8f;
    material.base_color_factor[1] = 0.8f;
    material.base_color_factor[2] = 0.8f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    const float nz = toward_light ? 1.0f : -1.0f;
    primitive.normals = {
        0.0f, 0.0f, nz, 0.0f, 0.0f, nz, 0.0f, 0.0f, nz,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "normal-lit-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_normal_texture_deferred_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData normal_texture;
    normal_texture.name = "normal-map-placeholder";
    normal_texture.mime_type = "image/png";
    normal_texture.encoded_bytes = {0x89, 0x50, 0x4e, 0x47};
    scene.textures.push_back(std::move(normal_texture));

    pulp::scene::MaterialData material;
    material.name = "normal-map-material";
    material.base_color_factor[0] = 0.7f;
    material.base_color_factor[1] = 0.7f;
    material.base_color_factor[2] = 0.7f;
    material.double_sided = true;
    material.normal_texture = 0;
    material.normal_scale = 0.6f;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.normals = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
    };
    primitive.tangents = {
        1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "normal-map-deferred-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_normal_texture_render_scene(float normal_scale = 1.0f,
                                                        bool tilted_normal = false) {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData normal_texture;
    normal_texture.name = tilted_normal ? "tilted-normal" : "reverse-z-normal";
    normal_texture.mime_type = "image/png";
    normal_texture.encoded_bytes =
        tilted_normal ? make_tilted_normal_png() : make_reverse_z_normal_png();
    scene.textures.push_back(std::move(normal_texture));

    pulp::scene::MaterialData material;
    material.name = "sampled-normal-map-material";
    material.base_color_factor[0] = 0.8f;
    material.base_color_factor[1] = 0.8f;
    material.base_color_factor[2] = 0.8f;
    material.double_sided = true;
    material.normal_texture = 0;
    material.normal_scale = normal_scale;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.normals = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
    };
    primitive.tangents = {
        1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f, 1.0f, 0.0f, 0.0f, 1.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "sampled-normal-map-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_normal_texture_derived_tangent_render_scene() {
    auto scene = make_normal_texture_render_scene();
    REQUIRE_FALSE(scene.meshes.empty());
    REQUIRE_FALSE(scene.meshes[0].primitives.empty());
    scene.meshes[0].primitives[0].tangents.clear();
    scene.materials[0].normal_scale = 0.8f;
    return scene;
}

pulp::scene::SceneData make_pbr_texture_slots_deferred_render_scene() {
    pulp::scene::SceneData scene;

    for (const char* name : {
             "metallic-roughness-placeholder",
             "normal-placeholder",
             "occlusion-placeholder",
             "emissive-placeholder",
         }) {
        pulp::scene::TextureData texture;
        texture.name = name;
        texture.mime_type = "image/png";
        texture.encoded_bytes = make_black_pixel_png();
        scene.textures.push_back(std::move(texture));
    }

    pulp::scene::MaterialData material;
    material.name = "deferred-pbr-texture-slots-material";
    material.base_color_factor[0] = 0.75f;
    material.base_color_factor[1] = 0.75f;
    material.base_color_factor[2] = 0.75f;
    material.double_sided = true;
    material.metallic_roughness_texture = 0;
    material.normal_texture = 1;
    material.occlusion_texture = 2;
    material.emissive_texture = 3;
    material.metallic_roughness_texcoord = 1;
    material.normal_texcoord = 1;
    material.occlusion_texcoord = 1;
    material.emissive_texcoord = 1;
    material.metallic_roughness_transform.enabled = true;
    material.metallic_roughness_transform.offset[0] = 0.2f;
    material.metallic_roughness_transform.scale[0] = 0.75f;
    material.normal_transform.enabled = true;
    material.normal_transform.rotation = 0.1f;
    material.occlusion_transform.enabled = true;
    material.occlusion_transform.offset[1] = 0.25f;
    material.emissive_transform.enabled = true;
    material.emissive_transform.scale[1] = 1.25f;
    material.normal_scale = 0.6f;
    material.occlusion_strength = 0.4f;
    material.emissive_factor[0] = 0.1f;
    material.emissive_factor[1] = 0.05f;
    material.emissive_factor[2] = 0.0f;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.normals = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.texcoord1 = {
        0.5f, 0.5f, 0.5f, 0.5f, 0.5f, 0.5f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "deferred-pbr-texture-slots-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

} // namespace

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
