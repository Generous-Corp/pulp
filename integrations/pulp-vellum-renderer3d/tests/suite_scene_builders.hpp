#pragma once

#include "suite_shared.hpp"

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic push
#pragma GCC diagnostic ignored "-Wunused-function"
#endif

namespace {

double foreground_center_x(const ForegroundRegion& region) {
    if (region.pixel_count == 0 || region.max_x < region.min_x) {
        return 0.0;
    }
    return (static_cast<double>(region.min_x) + static_cast<double>(region.max_x)) * 0.5;
}

pulp::scene::SceneData make_child_node_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "child-material";
    material.base_color_factor[0] = 0.25f;
    material.base_color_factor[1] = 0.75f;
    material.base_color_factor[2] = 0.35f;
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
    mesh.name = "child-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData root;
    root.name = "root";
    root.translation[0] = 3.0f;
    root.children.push_back(1);
    scene.nodes.push_back(std::move(root));

    pulp::scene::NodeData child;
    child.name = "child";
    child.mesh = 0;
    child.translation[1] = -2.0f;
    child.scale[0] = 0.75f;
    child.scale[1] = 1.25f;
    scene.nodes.push_back(std::move(child));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_transformed_uv_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData texture;
    texture.name = "fallback-base";
    texture.mime_type = "image/png";
    texture.encoded_bytes = {0x89, 0x50, 0x4e, 0x47};
    scene.textures.push_back(std::move(texture));

    pulp::scene::MaterialData material;
    material.name = "transformed-uv-material";
    material.base_color_texture = 0;
    material.base_color_texcoord = 0;
    material.base_color_transform.enabled = true;
    material.base_color_transform.offset[0] = 0.125f;
    material.base_color_transform.offset[1] = 0.25f;
    material.base_color_transform.scale[0] = 1.5f;
    material.base_color_transform.scale[1] = 0.5f;
    material.base_color_transform.rotation = 0.2f;
    material.base_color_transform.texcoord_override = 1;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f, 1.0f, -1.0f, 0.0f, 0.0f, 1.0f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.texcoord1 = {
        0.25f, 0.25f, 0.75f, 0.25f, 0.5f, 0.75f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "transformed-uv-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_mipmap_sampler_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::TextureData texture;
    texture.name = "fallback-with-mipmap-sampler";
    texture.mime_type = "image/png";
    texture.encoded_bytes = {0x89, 0x50, 0x4e, 0x47};
    scene.textures.push_back(std::move(texture));

    pulp::scene::TextureSamplerData sampler;
    sampler.name = "mipmap-linear";
    sampler.mag_filter = pulp::scene::TextureSamplerData::Filter::linear;
    sampler.min_filter = pulp::scene::TextureSamplerData::Filter::linear_mipmap_linear;
    scene.texture_samplers.push_back(sampler);

    pulp::scene::MaterialData material;
    material.name = "mipmap-sampler-material";
    material.base_color_texture = 0;
    material.base_color_sampler = 0;
    material.base_color_factor[0] = 0.8f;
    material.base_color_factor[1] = 0.8f;
    material.base_color_factor[2] = 0.8f;
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
    mesh.name = "mipmap-sampler-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_multi_primitive_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "multi-primitive-material";
    material.base_color_factor[0] = 0.85f;
    material.base_color_factor[1] = 0.45f;
    material.base_color_factor[2] = 0.20f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData left;
    left.positions = {
        -2.0f, -1.0f, 0.0f, -0.4f, -1.0f, 0.0f, -1.2f, 1.0f, 0.0f,
    };
    left.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    left.indices = {0, 1, 2};
    left.material = 0;

    pulp::scene::PrimitiveData right;
    right.positions = {
        0.4f, -1.0f, 0.0f, 2.0f, -1.0f, 0.0f, 1.2f, 1.0f, 0.0f,
    };
    right.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    right.indices = {0, 1, 2};
    right.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "two-triangle-mesh";
    mesh.primitives.push_back(std::move(left));
    mesh.primitives.push_back(std::move(right));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_mixed_material_primitive_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData red;
    red.name = "red-material";
    red.base_color_factor[0] = 1.0f;
    red.base_color_factor[1] = 0.05f;
    red.base_color_factor[2] = 0.05f;
    red.double_sided = true;
    scene.materials.push_back(red);

    pulp::scene::MaterialData green;
    green.name = "green-material";
    green.base_color_factor[0] = 0.05f;
    green.base_color_factor[1] = 1.0f;
    green.base_color_factor[2] = 0.05f;
    green.double_sided = true;
    scene.materials.push_back(green);

    pulp::scene::PrimitiveData left;
    left.positions = {
        -2.0f, -1.0f, 0.0f, -0.4f, -1.0f, 0.0f, -1.2f, 1.0f, 0.0f,
    };
    left.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    left.indices = {0, 1, 2};
    left.material = 0;

    pulp::scene::PrimitiveData right;
    right.positions = {
        0.4f, -1.0f, 0.0f, 2.0f, -1.0f, 0.0f, 1.2f, 1.0f, 0.0f,
    };
    right.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    right.indices = {0, 1, 2};
    right.material = 1;

    pulp::scene::MeshData mesh;
    mesh.name = "mixed-material-mesh";
    mesh.primitives.push_back(std::move(left));
    mesh.primitives.push_back(std::move(right));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

pulp::scene::SceneData make_directional_light_render_scene() {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "white-lit-material";
    material.base_color_factor[0] = 0.9f;
    material.base_color_factor[1] = 0.9f;
    material.base_color_factor[2] = 0.9f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::LightData light;
    light.name = "green-key";
    light.type = pulp::scene::LightData::Type::directional;
    light.color[0] = 0.05f;
    light.color[1] = 1.0f;
    light.color[2] = 0.05f;
    light.intensity = 1.0f;
    scene.lights.push_back(light);

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
    mesh.name = "directional-light-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData mesh_node;
    mesh_node.name = "mesh";
    mesh_node.mesh = 0;
    scene.nodes.push_back(std::move(mesh_node));
    scene.root_nodes.push_back(0);

    pulp::scene::NodeData light_node;
    light_node.name = "light";
    light_node.light = 0;
    scene.nodes.push_back(std::move(light_node));
    scene.root_nodes.push_back(1);

    return scene;
}

pulp::scene::SceneData make_directional_light_direction_render_scene(bool rotated_away) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "directional-light-direction-material";
    material.base_color_factor[0] = 0.9f;
    material.base_color_factor[1] = 0.9f;
    material.base_color_factor[2] = 0.9f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::LightData light;
    light.name = "white-key";
    light.type = pulp::scene::LightData::Type::directional;
    light.color[0] = 1.0f;
    light.color[1] = 1.0f;
    light.color[2] = 1.0f;
    light.intensity = 1.0f;
    scene.lights.push_back(light);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -0.65f, -0.65f, 0.0f, 0.65f, -0.65f, 0.0f, 0.0f, 0.65f, 0.0f,
    };
    primitive.normals = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f, 1.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "directional-light-direction-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData mesh_node;
    mesh_node.name = "mesh";
    mesh_node.mesh = 0;
    scene.nodes.push_back(std::move(mesh_node));
    scene.root_nodes.push_back(0);

    pulp::scene::NodeData light_node;
    light_node.name = "directional-light";
    light_node.light = 0;
    if (rotated_away) {
        light_node.rotation[1] = 1.0f;
        light_node.rotation[3] = 0.0f;
    }
    scene.nodes.push_back(std::move(light_node));
    scene.root_nodes.push_back(1);

    return scene;
}

pulp::scene::SceneData make_deferred_punctual_light_render_scene(bool attach_point_light = true,
                                                                 bool attach_spot_light = true) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "punctual-light-deferred-material";
    material.base_color_factor[0] = 0.85f;
    material.base_color_factor[1] = 0.85f;
    material.base_color_factor[2] = 0.85f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::LightData point_light;
    point_light.name = "deferred-point";
    point_light.type = pulp::scene::LightData::Type::point;
    point_light.color[0] = 1.0f;
    point_light.color[1] = 0.2f;
    point_light.color[2] = 0.2f;
    point_light.intensity = 2.0f;
    point_light.range = 6.0f;
    scene.lights.push_back(point_light);

    pulp::scene::LightData spot_light;
    spot_light.name = "deferred-spot";
    spot_light.type = pulp::scene::LightData::Type::spot;
    spot_light.color[0] = 0.2f;
    spot_light.color[1] = 0.2f;
    spot_light.color[2] = 1.0f;
    spot_light.intensity = 3.0f;
    spot_light.range = 8.0f;
    spot_light.inner_cone_angle = 0.2f;
    spot_light.outer_cone_angle = 0.7f;
    scene.lights.push_back(spot_light);

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
    mesh.name = "punctual-light-deferred-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData mesh_node;
    mesh_node.name = "mesh";
    mesh_node.mesh = 0;
    scene.nodes.push_back(std::move(mesh_node));
    scene.root_nodes.push_back(0);

    if (attach_point_light) {
        pulp::scene::NodeData point_node;
        point_node.name = "point-light";
        point_node.light = 0;
        point_node.translation[0] = 1.0f;
        point_node.translation[2] = 2.0f;
        scene.nodes.push_back(std::move(point_node));
        scene.root_nodes.push_back(static_cast<uint32_t>(scene.nodes.size() - 1u));
    }

    if (attach_spot_light) {
        pulp::scene::NodeData spot_node;
        spot_node.name = "spot-light";
        spot_node.light = 1;
        spot_node.translation[0] = -1.0f;
        spot_node.translation[2] = 2.0f;
        scene.nodes.push_back(std::move(spot_node));
        scene.root_nodes.push_back(static_cast<uint32_t>(scene.nodes.size() - 1u));
    }

    return scene;
}

pulp::scene::SceneData make_point_light_range_render_scene(float range) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "point-range-material";
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
    mesh.name = "point-range-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData mesh_node;
    mesh_node.name = "mesh";
    mesh_node.mesh = 0;
    scene.nodes.push_back(std::move(mesh_node));
    scene.root_nodes.push_back(0);

    pulp::scene::LightData point_light;
    point_light.name = "point-range";
    point_light.type = pulp::scene::LightData::Type::point;
    point_light.color[0] = 1.0f;
    point_light.color[1] = 0.25f;
    point_light.color[2] = 0.25f;
    point_light.intensity = 2.5f;
    point_light.range = range;
    scene.lights.push_back(point_light);

    pulp::scene::NodeData point_node;
    point_node.name = "point-light";
    point_node.light = 0;
    point_node.translation[2] = 2.0f;
    scene.nodes.push_back(std::move(point_node));
    scene.root_nodes.push_back(static_cast<uint32_t>(scene.nodes.size() - 1u));

    return scene;
}

pulp::scene::SceneData make_transformed_camera_light_render_scene() {
    auto scene = make_perspective_camera_render_scene(true);

    pulp::scene::LightData light;
    light.name = "rotated-key";
    light.type = pulp::scene::LightData::Type::directional;
    light.color[0] = 0.8f;
    light.color[1] = 0.8f;
    light.color[2] = 1.0f;
    light.intensity = 1.2f;
    scene.lights.push_back(light);

    for (auto& node : scene.nodes) {
        if (node.camera == 0) {
            node.translation[2] = 4.0f;
            node.rotation[1] = 0.258819f;
            node.rotation[3] = 0.965926f;
        }
    }

    pulp::scene::NodeData light_node;
    light_node.name = "rotated-light";
    light_node.light = 0;
    light_node.rotation[0] = 0.382683f;
    light_node.rotation[3] = 0.92388f;
    scene.nodes.push_back(std::move(light_node));
    scene.root_nodes.push_back(static_cast<uint32_t>(scene.nodes.size() - 1u));

    return scene;
}

pulp::scene::SceneData make_translated_camera_render_scene(float camera_x) {
    auto scene = make_perspective_camera_render_scene(true);
    REQUIRE_FALSE(scene.meshes.empty());
    REQUIRE_FALSE(scene.meshes[0].primitives.empty());
    scene.meshes[0].primitives[0].positions = {
        -0.35f, -0.35f, 0.0f, 0.35f, -0.35f, 0.0f, 0.0f, 0.35f, 0.0f,
    };
    for (auto& node : scene.nodes) {
        if (node.camera == 0) {
            node.translation[0] = camera_x;
        }
    }
    return scene;
}

pulp::scene::SceneData make_rotated_camera_render_scene(float camera_yaw_half_sin,
                                                        float camera_yaw_half_cos) {
    auto scene = make_translated_camera_render_scene(-0.25f);
    for (auto& node : scene.nodes) {
        if (node.camera == 0) {
            node.rotation[1] = camera_yaw_half_sin;
            node.rotation[3] = camera_yaw_half_cos;
        }
    }
    return scene;
}

pulp::scene::SceneData make_matrix_camera_render_scene(bool rotated) {
    auto scene = make_translated_camera_render_scene(-0.25f);
    for (auto& node : scene.nodes) {
        if (node.camera == 0) {
            node.has_matrix_transform = true;
            for (size_t i = 0; i < 16; ++i) {
                node.matrix[i] = 0.0f;
            }
            node.matrix[0] = 1.0f;
            node.matrix[5] = 1.0f;
            node.matrix[10] = 1.0f;
            node.matrix[15] = 1.0f;
            node.matrix[12] = -0.25f;
            if (rotated) {
                constexpr float c = 0.70710677f;
                constexpr float s = 0.70710677f;
                node.matrix[0] = c;
                node.matrix[2] = -s;
                node.matrix[8] = s;
                node.matrix[10] = c;
            }
        }
    }
    return scene;
}

pulp::scene::SceneData make_camera_metadata_render_scene() {
    auto scene = make_perspective_camera_render_scene(true);
    REQUIRE_FALSE(scene.cameras.empty());
    scene.cameras[0].aspect_ratio = 1.75f;
    scene.cameras[0].znear = 0.25f;
    scene.cameras[0].zfar = 64.0f;
    return scene;
}

pulp::scene::SceneData make_camera_aspect_ratio_render_scene(float aspect_ratio) {
    auto scene = make_perspective_camera_render_scene(true);
    REQUIRE_FALSE(scene.cameras.empty());
    scene.cameras[0].aspect_ratio = aspect_ratio;
    REQUIRE_FALSE(scene.meshes.empty());
    REQUIRE_FALSE(scene.meshes[0].primitives.empty());
    scene.meshes[0].primitives[0].positions = {
        -0.45f, -0.45f, 0.0f, 0.45f, -0.45f, 0.0f, 0.0f, 0.45f, 0.0f,
    };
    return scene;
}

pulp::scene::SceneData make_orthographic_camera_render_scene(float ymag) {
    pulp::scene::SceneData scene;

    pulp::scene::MaterialData material;
    material.name = "orthographic-camera-scale-material";
    material.base_color_factor[0] = 0.85f;
    material.base_color_factor[1] = 0.85f;
    material.base_color_factor[2] = 0.85f;
    material.double_sided = true;
    scene.materials.push_back(material);

    pulp::scene::PrimitiveData primitive;
    primitive.positions = {
        -0.65f, -0.65f, 0.0f, 0.65f, -0.65f, 0.0f, 0.0f, 0.65f, 0.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f, 1.0f, 0.0f, 0.5f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    pulp::scene::MeshData mesh;
    mesh.name = "orthographic-camera-scale-triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    pulp::scene::NodeData mesh_node;
    mesh_node.name = "mesh";
    mesh_node.mesh = 0;
    scene.nodes.push_back(std::move(mesh_node));
    scene.root_nodes.push_back(0);

    pulp::scene::CameraData camera;
    camera.name = "orthographic-camera";
    camera.projection = pulp::scene::CameraData::Projection::orthographic;
    camera.xmag = ymag;
    camera.ymag = ymag;
    camera.znear = 0.1f;
    camera.zfar = 10.0f;
    scene.cameras.push_back(camera);

    pulp::scene::NodeData camera_node;
    camera_node.name = "camera";
    camera_node.camera = 0;
    scene.nodes.push_back(std::move(camera_node));
    scene.root_nodes.push_back(1);

    return scene;
}

std::vector<uint8_t> make_external_renderable_gltf_bin() {
    std::vector<uint8_t> bin;
    for (float value : {
             -1.0f,
             -1.0f,
             0.0f,
             1.0f,
             -1.0f,
             0.0f,
             0.0f,
             1.0f,
             0.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f,
             0.0f,
             1.0f,
             0.0f,
             0.0f,
             1.0f,
             0.0f,
             0.0f,
             1.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f,
             0.0f,
             1.0f,
             0.0f,
             0.5f,
             1.0f,
         }) {
        append_f32(bin, value);
    }
    append_u16(bin, 0);
    append_u16(bin, 1);
    append_u16(bin, 2);
    pad4(bin);
    return bin;
}

std::string make_external_renderable_gltf_json(size_t bin_byte_length) {
    return R"({"asset":{"version":"2.0","generator":"pulp-test-renderer3d-external"},"buffers":[{"uri":"mesh.bin","byteLength":)" +
           std::to_string(bin_byte_length) +
           R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":36,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":72,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":96,"byteLength":6,"target":34963}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],"images":[{"uri":"checker.png","mimeType":"image/png","name":"external-checker"}],"samplers":[{"name":"externalLinearClamp","magFilter":9729,"minFilter":9729,"wrapS":33071,"wrapT":33071}],"textures":[{"source":0,"sampler":0,"name":"externalCheckerTexture"}],"materials":[{"name":"external-mat","pbrMetallicRoughness":{"baseColorFactor":[0.8,0.5,0.25,1],"baseColorTexture":{"index":0}},"doubleSided":true}],"meshes":[{"name":"external-triangle","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0,"mode":4}]}],"nodes":[{"name":"external-root","mesh":0,"translation":[-1,0,0]}],"scenes":[{"nodes":[0]}],"scene":0})";
}
} // namespace

#if defined(__clang__) || defined(__GNUC__)
#pragma GCC diagnostic pop
#endif
