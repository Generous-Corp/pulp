#include <catch2/catch_test_macros.hpp>
#include <pulp/scene/bake_preflight.hpp>
#include <pulp/scene/gltf_loader.hpp>
#include <pulp/scene/material_key.hpp>
#include <pulp/scene/render_packet.hpp>
#include <pulp/scene/scene_data.hpp>
#include <pulp/scene/scene_graph.hpp>
#include <pulp/scene/scene_stats.hpp>
#include <pulp/scene/sidecar.hpp>

#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <utility>
#include <vector>

using namespace pulp::scene;

namespace {

SceneData make_textured_triangle_scene() {
    SceneData scene;

    TextureData texture;
    texture.name = "checker";
    texture.mime_type = "image/png";
    texture.encoded_bytes = {0x89, 0x50, 0x4e, 0x47};
    scene.textures.push_back(std::move(texture));

    MaterialData material;
    material.name = "mat";
    material.base_color_texture = 0;
    scene.materials.push_back(std::move(material));

    PrimitiveData primitive;
    primitive.positions = {
        -1.0f, -1.0f, 0.0f,
         1.0f, -1.0f, 0.0f,
         0.0f,  1.0f, 0.0f,
    };
    primitive.normals = {
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f,
    };
    primitive.tangents = {
        1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f,
        1.0f, 0.0f, 0.0f, 1.0f,
    };
    primitive.texcoord0 = {
        0.0f, 0.0f,
        1.0f, 0.0f,
        0.5f, 1.0f,
    };
    primitive.texcoord1 = {
        0.25f, 0.25f,
        0.75f, 0.25f,
        0.5f, 0.75f,
    };
    primitive.color0 = {
        1.0f, 0.0f, 0.0f, 1.0f,
        0.0f, 1.0f, 0.0f, 1.0f,
        0.0f, 0.0f, 1.0f, 1.0f,
    };
    primitive.indices = {0, 1, 2};
    primitive.material = 0;

    MeshData mesh;
    mesh.name = "triangle";
    mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(mesh));

    NodeData node;
    node.name = "root";
    node.mesh = 0;
    scene.nodes.push_back(std::move(node));
    scene.root_nodes.push_back(0);

    return scene;
}

bool has_code(const std::vector<Diagnostic>& diagnostics, const std::string& code) {
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) {
            return true;
        }
    }
    return false;
}

bool has_unsupported_feature(const SidecarData& sidecar,
                             const std::string& feature) {
    for (const auto& unsupported : sidecar.unsupported_features) {
        if (unsupported.feature == feature) {
            return true;
        }
    }
    return false;
}

bool has_code_path(const std::vector<Diagnostic>& diagnostics,
                   const std::string& code,
                   const std::filesystem::path& source_path) {
    const auto expected = source_path.string();
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code &&
            diagnostic.source_path == expected) {
            return true;
        }
    }
    return false;
}

std::string make_minimal_sidecar_json(const std::string& source,
                                      const std::string& exporter,
                                      const std::string& exported_at,
                                      const std::string& runtime_evidence) {
    return std::string(
               R"({"schema_version":1,"provenance":{"source":")") +
           source +
           R"(","exporter":")" +
           exporter +
           R"(","exported_at":")" +
           exported_at +
           R"(","runtime_evidence":")" +
           runtime_evidence +
           R"("},"diagnostics":[],"unsupported_features":[],"runtime_hints":[]})";
}

size_t count_code(const std::vector<Diagnostic>& diagnostics, const std::string& code) {
    size_t count = 0;
    for (const auto& diagnostic : diagnostics) {
        if (diagnostic.code == code) {
            ++count;
        }
    }
    return count;
}

void append_u32(std::vector<uint8_t>& bytes, uint32_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 16u) & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 24u) & 0xffu));
}

void append_f32(std::vector<uint8_t>& bytes, float value) {
    uint32_t raw = 0;
    std::memcpy(&raw, &value, sizeof(raw));
    append_u32(bytes, raw);
}

void append_u16(std::vector<uint8_t>& bytes, uint16_t value) {
    bytes.push_back(static_cast<uint8_t>(value & 0xffu));
    bytes.push_back(static_cast<uint8_t>((value >> 8u) & 0xffu));
}

void pad4(std::vector<uint8_t>& bytes, uint8_t fill = 0) {
    while (bytes.size() % 4u != 0u) {
        bytes.push_back(fill);
    }
}

std::vector<uint8_t> make_minimal_textured_glb() {
    std::vector<uint8_t> bin;
    for (float value : {
             -1.0f, -1.0f, 0.0f,
              1.0f, -1.0f, 0.0f,
              0.0f,  1.0f, 0.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f, 0.0f, 1.0f,
             0.0f, 0.0f, 1.0f,
             0.0f, 0.0f, 1.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             1.0f, 0.0f, 0.0f, 1.0f,
             1.0f, 0.0f, 0.0f, 1.0f,
             1.0f, 0.0f, 0.0f, 1.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f, 0.0f,
             1.0f, 0.0f,
             0.5f, 1.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.25f, 0.25f,
             0.75f, 0.25f,
             0.5f, 0.75f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             1.0f, 0.0f, 0.0f, 1.0f,
             0.0f, 1.0f, 0.0f, 1.0f,
             0.0f, 0.0f, 1.0f, 1.0f,
         }) {
        append_f32(bin, value);
    }
    append_u16(bin, 0);
    append_u16(bin, 1);
    append_u16(bin, 2);
    pad4(bin);

    const std::vector<uint8_t> fake_png = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    };
    bin.insert(bin.end(), fake_png.begin(), fake_png.end());
    pad4(bin);

    const std::string json =
        R"({"asset":{"version":"2.0","generator":"pulp-test-scene3d"},"buffers":[{"byteLength":232}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":36,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":72,"byteLength":48,"target":34962},{"buffer":0,"byteOffset":120,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":144,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":168,"byteLength":48,"target":34962},{"buffer":0,"byteOffset":216,"byteLength":6,"target":34963},{"buffer":0,"byteOffset":224,"byteLength":8}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC4"},{"bufferView":3,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":4,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":5,"componentType":5126,"count":3,"type":"VEC4"},{"bufferView":6,"componentType":5123,"count":3,"type":"SCALAR"}],"images":[{"bufferView":7,"mimeType":"image/png","name":"checker"}],"textures":[{"source":0,"name":"checkerTexture"}],"materials":[{"name":"mat","pbrMetallicRoughness":{"baseColorFactor":[0.25,0.5,0.75,1],"baseColorTexture":{"index":0}},"doubleSided":true}],"meshes":[{"name":"triangle","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TANGENT":2,"TEXCOORD_0":3,"TEXCOORD_1":4,"COLOR_0":5},"indices":6,"material":0,"mode":4}]}],"nodes":[{"name":"root","mesh":0,"translation":[1,2,3]}],"scenes":[{"nodes":[0]}],"scene":0})";

    std::vector<uint8_t> json_bytes(json.begin(), json.end());
    pad4(json_bytes, 0x20);

    std::vector<uint8_t> glb;
    append_u32(glb, 0x46546c67u);
    append_u32(glb, 2u);
    append_u32(glb, static_cast<uint32_t>(12u + 8u + json_bytes.size() + 8u + bin.size()));
    append_u32(glb, static_cast<uint32_t>(json_bytes.size()));
    append_u32(glb, 0x4e4f534au);
    glb.insert(glb.end(), json_bytes.begin(), json_bytes.end());
    append_u32(glb, static_cast<uint32_t>(bin.size()));
    append_u32(glb, 0x004e4942u);
    glb.insert(glb.end(), bin.begin(), bin.end());
    return glb;
}

std::vector<uint8_t> make_minimal_external_gltf_bin() {
    std::vector<uint8_t> bin;
    for (float value : {
             -1.0f, -1.0f, 0.0f,
              1.0f, -1.0f, 0.0f,
              0.0f,  1.0f, 0.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f, 0.0f, 1.0f,
             0.0f, 0.0f, 1.0f,
             0.0f, 0.0f, 1.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f, 0.0f,
             1.0f, 0.0f,
             0.5f, 1.0f,
         }) {
        append_f32(bin, value);
    }
    append_u16(bin, 0);
    append_u16(bin, 1);
    append_u16(bin, 2);
    pad4(bin);
    for (uint16_t value : {
             uint16_t{0}, uint16_t{0}, uint16_t{0}, uint16_t{0},
             uint16_t{0}, uint16_t{0}, uint16_t{0}, uint16_t{0},
             uint16_t{0}, uint16_t{0}, uint16_t{0}, uint16_t{0},
         }) {
        append_u16(bin, value);
    }
    for (float value : {
             1.0f, 0.0f, 0.0f, 0.0f,
             1.0f, 0.0f, 0.0f, 0.0f,
             1.0f, 0.0f, 0.0f, 0.0f,
         }) {
        append_f32(bin, value);
    }
    return bin;
}

std::string make_minimal_external_gltf_json(size_t bin_byte_length) {
    return R"({"asset":{"version":"2.0","generator":"pulp-test-scene3d-external"},"buffers":[{"uri":"mesh.bin","byteLength":)" +
        std::to_string(bin_byte_length) +
        R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":36,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":72,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":96,"byteLength":6,"target":34963}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],"images":[{"uri":"checker.png","mimeType":"image/png","name":"external-checker"}],"textures":[{"source":0,"name":"checkerTexture"}],"materials":[{"name":"external-mat","pbrMetallicRoughness":{"baseColorFactor":[1,0.5,0.25,1],"baseColorTexture":{"index":0}}}],"meshes":[{"name":"external-triangle","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"material":0,"mode":4}]}],"nodes":[{"name":"external-root","mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
}

std::string make_camera_light_gltf_json() {
    return R"({"asset":{"version":"2.0","generator":"pulp-test-scene3d-camera-light"},"extensionsUsed":["KHR_lights_punctual"],"extensions":{"KHR_lights_punctual":{"lights":[{"name":"Key","type":"directional","color":[1,0.8,0.6],"intensity":2.5},{"name":"Fill","type":"point","color":[0.25,0.5,1],"intensity":4,"range":9},{"name":"Spot","type":"spot","color":[1,1,1],"intensity":3,"range":12,"spot":{"innerConeAngle":0.2,"outerConeAngle":0.7}}]}},"cameras":[{"name":"MainCamera","type":"perspective","perspective":{"aspectRatio":1.5,"yfov":0.75,"znear":0.1,"zfar":50}},{"name":"OrthoCamera","type":"orthographic","orthographic":{"xmag":4,"ymag":3,"znear":0.2,"zfar":20}}],"nodes":[{"name":"camera-node","camera":0},{"name":"light-node","extensions":{"KHR_lights_punctual":{"light":0}}},{"name":"spot-node","extensions":{"KHR_lights_punctual":{"light":2}}},{"name":"ortho-node","camera":1}],"scenes":[{"nodes":[0,1,2,3]}],"scene":0})";
}

std::string make_pbr_material_gltf_json() {
    return R"({"asset":{"version":"2.0","generator":"pulp-test-scene3d-pbr-material"},"extensionsUsed":["KHR_texture_transform"],"samplers":[{"name":"nearestClamp","magFilter":9728,"minFilter":9984,"wrapS":33071,"wrapT":33648},{"name":"linearRepeat","magFilter":9729,"minFilter":9987,"wrapS":10497,"wrapT":10497}],"images":[{"uri":"base.png","mimeType":"image/png","name":"base"},{"uri":"metalrough.png","mimeType":"image/png","name":"metalrough"},{"uri":"normal.png","mimeType":"image/png","name":"normal"},{"uri":"occlusion.png","mimeType":"image/png","name":"occlusion"},{"uri":"emissive.png","mimeType":"image/png","name":"emissive"}],"textures":[{"source":0,"sampler":0},{"source":1,"sampler":1},{"source":2,"sampler":1},{"source":3,"sampler":0},{"source":4,"sampler":1}],"materials":[{"name":"PBRMat","pbrMetallicRoughness":{"baseColorFactor":[0.2,0.4,0.6,0.8],"metallicFactor":0.7,"roughnessFactor":0.35,"baseColorTexture":{"index":0,"texCoord":1,"extensions":{"KHR_texture_transform":{"offset":[0.25,0.5],"scale":[2,3],"rotation":0.125,"texCoord":1}}},"metallicRoughnessTexture":{"index":1,"texCoord":1}},"normalTexture":{"index":2,"texCoord":1,"scale":0.65},"occlusionTexture":{"index":3,"texCoord":1,"strength":0.4},"emissiveTexture":{"index":4,"texCoord":1},"emissiveFactor":[0.1,0.2,0.3],"alphaMode":"MASK","alphaCutoff":0.42,"doubleSided":true}],"scenes":[{}],"scene":0})";
}

std::string make_advanced_material_gltf_json() {
    return R"({"asset":{"version":"2.0","generator":"pulp-test-scene3d-advanced-material"},"extensionsUsed":["KHR_materials_clearcoat","KHR_materials_transmission","KHR_materials_sheen","KHR_materials_specular","KHR_materials_volume","KHR_materials_anisotropy","KHR_materials_iridescence","KHR_materials_diffuse_transmission","KHR_materials_ior","KHR_materials_dispersion"],"materials":[{"name":"PhysicalMat","pbrMetallicRoughness":{"baseColorFactor":[1,1,1,1]},"extensions":{"KHR_materials_clearcoat":{"clearcoatFactor":0.5},"KHR_materials_transmission":{"transmissionFactor":0.25},"KHR_materials_sheen":{"sheenRoughnessFactor":0.5},"KHR_materials_specular":{"specularFactor":0.8},"KHR_materials_volume":{"thicknessFactor":0.2},"KHR_materials_anisotropy":{"anisotropyStrength":0.4},"KHR_materials_iridescence":{"iridescenceFactor":0.3},"KHR_materials_diffuse_transmission":{"diffuseTransmissionFactor":0.2},"KHR_materials_ior":{"ior":1.4},"KHR_materials_dispersion":{"dispersion":0.1}}}],"scenes":[{}],"scene":0})";
}

std::vector<uint8_t> make_unsupported_mesh_features_gltf_bin() {
    std::vector<uint8_t> bin;
    for (float value : {
             -1.0f, -1.0f, 0.0f,
              1.0f, -1.0f, 0.0f,
              0.0f,  1.0f, 0.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             -0.8f, -1.0f, 0.0f,
              1.2f, -1.0f, 0.0f,
              0.0f,  1.2f, 0.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f, 0.0f, 0.0f,
             2.0f, 0.0f, 0.0f,
         }) {
        append_f32(bin, value);
    }
    append_u16(bin, 0);
    append_u16(bin, 1);
    append_u16(bin, 2);
    pad4(bin);
    return bin;
}

std::string make_unsupported_mesh_features_gltf_json(size_t bin_byte_length) {
    return R"({"asset":{"version":"2.0","generator":"pulp-test-scene3d-unsupported-mesh-features"},"extensionsUsed":["EXT_mesh_gpu_instancing"],"buffers":[{"uri":"unsupported.bin","byteLength":)" +
        std::to_string(bin_byte_length) +
        R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":36,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":72,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":96,"byteLength":6,"target":34963},{"buffer":0,"byteOffset":104,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":128,"byteLength":48,"target":34962}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3","min":[-0.8,-1,0],"max":[1.2,1.2,0]},{"bufferView":2,"componentType":5126,"count":2,"type":"VEC3","min":[0,0,0],"max":[2,0,0]},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"},{"bufferView":4,"componentType":5123,"count":3,"type":"VEC4"},{"bufferView":5,"componentType":5126,"count":3,"type":"VEC4"}],"meshes":[{"name":"MorphMesh","weights":[0.5],"primitives":[{"attributes":{"POSITION":0,"JOINTS_0":4,"WEIGHTS_0":5},"targets":[{"POSITION":1}],"indices":3,"mode":4}]}],"nodes":[{"name":"SkinnedMorphed","mesh":0,"skin":0},{"name":"Joint"},{"name":"InstancedNode","mesh":0,"extensions":{"EXT_mesh_gpu_instancing":{"attributes":{"TRANSLATION":2}}}}],"skins":[{"name":"Skin","joints":[1]}],"scenes":[{"nodes":[0,1,2]}],"scene":0})";
}

std::vector<uint8_t> make_animation_gltf_bin() {
    std::vector<uint8_t> bin;
    append_f32(bin, 0.0f);
    append_f32(bin, 1.0f);

    for (float value : {
             0.0f, 0.0f, 0.0f,
             2.0f, 3.0f, 4.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f, 0.0f, 0.0f, 1.0f,
             0.0f, 0.70710677f, 0.0f, 0.70710677f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             1.0f, 1.0f, 1.0f,
             2.0f, 2.0f, 2.0f,
         }) {
        append_f32(bin, value);
    }
    for (float value : {
             0.0f, 1.0f,
             1.0f, 0.0f,
         }) {
        append_f32(bin, value);
    }
    return bin;
}

std::string make_animation_gltf_json(size_t bin_byte_length) {
    return R"({"asset":{"version":"2.0","generator":"pulp-test-scene3d-animation"},"buffers":[{"uri":"anim.bin","byteLength":)" +
        std::to_string(bin_byte_length) +
        R"(}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":8},{"buffer":0,"byteOffset":8,"byteLength":24},{"buffer":0,"byteOffset":32,"byteLength":32},{"buffer":0,"byteOffset":64,"byteLength":24},{"buffer":0,"byteOffset":88,"byteLength":16}],"accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},{"bufferView":1,"componentType":5126,"count":2,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":2,"type":"VEC4"},{"bufferView":3,"componentType":5126,"count":2,"type":"VEC3"},{"bufferView":4,"componentType":5126,"count":2,"type":"VEC2"}],"nodes":[{"name":"animated"}],"animations":[{"name":"MoveSpinScale","samplers":[{"input":0,"output":1,"interpolation":"LINEAR"},{"input":0,"output":2,"interpolation":"STEP"},{"input":0,"output":3,"interpolation":"LINEAR"},{"input":0,"output":4,"interpolation":"LINEAR"}],"channels":[{"sampler":0,"target":{"node":0,"path":"translation"}},{"sampler":1,"target":{"node":0,"path":"rotation"}},{"sampler":2,"target":{"node":0,"path":"scale"}},{"sampler":3,"target":{"node":0,"path":"weights"}}]}],"scenes":[{"nodes":[0]}],"scene":0})";
}

std::filesystem::path write_temp_file(const std::string& name,
                                      const std::vector<uint8_t>& bytes) {
    auto path = std::filesystem::temp_directory_path() / name;
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
    return path;
}

void write_file(const std::filesystem::path& path, const std::vector<uint8_t>& bytes) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(reinterpret_cast<const char*>(bytes.data()),
               static_cast<std::streamsize>(bytes.size()));
    REQUIRE(file.good());
}

void write_file(const std::filesystem::path& path, const std::string& text) {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
    file.write(text.data(), static_cast<std::streamsize>(text.size()));
    REQUIRE(file.good());
}

std::filesystem::path scene3d_fixture_path(const std::string& relative) {
#ifdef PULP_TEST_SCENE3D_FIXTURE_DIR
    return std::filesystem::path(PULP_TEST_SCENE3D_FIXTURE_DIR) / relative;
#else
    return std::filesystem::path("test/fixtures/scene3d") / relative;
#endif
}

} // namespace

TEST_CASE("SceneData valid textured primitive has no structural diagnostics",
          "[scene3d][scene]") {
    auto scene = make_textured_triangle_scene();

    auto diagnostics = validate_scene_data(scene, "fixture.glb");

    REQUIRE(diagnostics.empty());
    REQUIRE_FALSE(scene.empty());
    REQUIRE(is_valid_scene_index(0, scene.nodes.size()));
    REQUIRE(diagnostic_severity_name(Diagnostic::Severity::warning) ==
            std::string("warning"));
}

TEST_CASE("SceneData validation reports loader-boundary contract violations",
          "[scene3d][scene]") {
    auto scene = make_textured_triangle_scene();
    scene.root_nodes.push_back(99);
    scene.nodes[0].children.push_back(4);
    scene.nodes[0].camera = 9;
    scene.nodes[0].light = 7;
    scene.materials[0].base_color_texture = 5;
    scene.materials[0].base_color_sampler = 3;
    scene.meshes[0].primitives[0].indices = {0, 8, 1};
    scene.meshes[0].primitives[0].tangents.pop_back();
    scene.meshes[0].primitives[0].texcoord0.pop_back();
    scene.meshes[0].primitives[0].texcoord1.pop_back();
    scene.meshes[0].primitives[0].color0.pop_back();

    auto diagnostics = validate_scene_data(scene, "bad.glb");

    REQUIRE(has_error_diagnostics(diagnostics));
    REQUIRE(has_code(diagnostics, "scene.root_node_out_of_range"));
    REQUIRE(has_code(diagnostics, "scene.node_child_out_of_range"));
    REQUIRE(has_code(diagnostics, "scene.node_camera_out_of_range"));
    REQUIRE(has_code(diagnostics, "scene.node_light_out_of_range"));
    REQUIRE(has_code(diagnostics, "scene.material_texture_out_of_range"));
    REQUIRE(has_code(diagnostics, "scene.material_sampler_out_of_range"));
    REQUIRE(has_code(diagnostics, "scene.primitive_index_out_of_range"));
    REQUIRE(has_code(diagnostics, "scene.primitive_tangent_count_mismatch"));
    REQUIRE(has_code(diagnostics, "scene.primitive_texcoord_count_mismatch"));
    REQUIRE(has_code(diagnostics, "scene.primitive_color_count_mismatch"));
}

TEST_CASE("SceneData validation covers every material texture and sampler slot",
          "[scene3d][scene]") {
    auto scene = make_textured_triangle_scene();
    scene.materials[0].base_color_texture = 10;
    scene.materials[0].metallic_roughness_texture = 11;
    scene.materials[0].normal_texture = 12;
    scene.materials[0].occlusion_texture = 13;
    scene.materials[0].emissive_texture = 14;
    scene.materials[0].base_color_sampler = 20;
    scene.materials[0].metallic_roughness_sampler = 21;
    scene.materials[0].normal_sampler = 22;
    scene.materials[0].occlusion_sampler = 23;
    scene.materials[0].emissive_sampler = 24;

    const auto diagnostics = validate_scene_data(scene, "material-slots.glb");

    REQUIRE(has_error_diagnostics(diagnostics));
    REQUIRE(count_code(diagnostics, "scene.material_texture_out_of_range") == 5);
    REQUIRE(count_code(diagnostics, "scene.material_sampler_out_of_range") == 5);
}

TEST_CASE("SceneData validation warns on empty meshes and requires indexed primitives",
          "[scene3d][scene]") {
    SceneData scene;

    MeshData empty_mesh;
    empty_mesh.name = "empty";
    scene.meshes.push_back(std::move(empty_mesh));

    MeshData bad_mesh;
    PrimitiveData primitive;
    primitive.positions = {
        0.0f, 0.0f, 0.0f,
        1.0f, 0.0f, 0.0f,
        0.0f, 1.0f, 0.0f,
    };
    bad_mesh.primitives.push_back(std::move(primitive));
    scene.meshes.push_back(std::move(bad_mesh));

    auto diagnostics = validate_scene_data(scene);

    REQUIRE(has_code(diagnostics, "scene.mesh_without_primitives"));
    REQUIRE(has_code(diagnostics, "scene.primitive_missing_indices"));
    REQUIRE(has_error_diagnostics(diagnostics));
}

TEST_CASE("SceneData stats summarize the native loader inspection surface",
          "[scene3d][scene][stats]") {
    auto scene = make_textured_triangle_scene();
    scene.texture_samplers.push_back(TextureSamplerData{});
    scene.materials[0].base_color_sampler = 0;
    scene.materials[0].advanced_material_extensions.push_back(
        "KHR_materials_clearcoat");
    scene.materials[0].advanced_material_extensions.push_back(
        "KHR_materials_transmission");
    append_diagnostic(scene.diagnostics,
                      Diagnostic::Severity::warning,
                      "scene.test_warning",
                      "warning for stats test");

    const auto stats = summarize_scene_data(scene);

    REQUIRE(stats.nodes == 1);
    REQUIRE(stats.root_nodes == 1);
    REQUIRE(stats.meshes == 1);
    REQUIRE(stats.primitives == 1);
    REQUIRE(stats.indexed_primitives == 1);
    REQUIRE(stats.vertices == 3);
    REQUIRE(stats.indices == 3);
    REQUIRE(stats.materials == 1);
    REQUIRE(stats.textures == 1);
    REQUIRE(stats.texture_samplers == 1);
    REQUIRE(stats.texture_bytes == 4);
    REQUIRE(stats.advanced_material_extensions == 2);
    REQUIRE(stats.cameras == 0);
    REQUIRE(stats.lights == 0);
    REQUIRE(stats.animations == 0);
    REQUIRE(stats.unsupported_features == 0);
    REQUIRE(stats.diagnostics == 1);
    REQUIRE(stats.error_diagnostics == 0);

    const auto text = scene_stats_to_text(stats);
    REQUIRE(text.find("nodes=1") != std::string::npos);
    REQUIRE(text.find("vertices=3") != std::string::npos);
    REQUIRE(text.find("indices=3") != std::string::npos);
    REQUIRE(text.find("texture_samplers=1") != std::string::npos);
    REQUIRE(text.find("advanced_material_extensions=2") != std::string::npos);
    REQUIRE(text.find("error_diagnostics=0") != std::string::npos);
}

TEST_CASE("Scene graph traversal composes parent and child node transforms",
          "[scene3d][scene]") {
    auto scene = make_textured_triangle_scene();
    scene.root_nodes.clear();
    scene.nodes.clear();

    NodeData root;
    root.name = "group";
    root.translation[0] = 10.0f;
    root.children.push_back(1);
    scene.nodes.push_back(std::move(root));

    NodeData child;
    child.name = "mesh-node";
    child.mesh = 0;
    child.translation[1] = 2.0f;
    child.scale[0] = 2.0f;
    child.scale[1] = 3.0f;
    child.scale[2] = 4.0f;
    scene.nodes.push_back(std::move(child));
    scene.root_nodes.push_back(0);

    std::vector<Diagnostic> diagnostics;
    const auto renderables = collect_renderable_nodes(scene, &diagnostics);

    REQUIRE_FALSE(has_error_diagnostics(diagnostics));
    REQUIRE(renderables.size() == 1);
    REQUIRE(renderables[0].node == 1);
    REQUIRE(renderables[0].mesh == 0);

    const auto point = transform_point(renderables[0].world_transform,
                                       1.0f,
                                       1.0f,
                                       1.0f);
    REQUIRE(point.x == 12.0f);
    REQUIRE(point.y == 5.0f);
    REQUIRE(point.z == 4.0f);

    const auto transformed_nodes = collect_node_world_transforms(scene);
    REQUIRE(transformed_nodes.size() == 2);
    REQUIRE(transformed_nodes[0].node == 0);
    REQUIRE(transformed_nodes[1].node == 1);
    const auto child_origin = transform_point(transformed_nodes[1].world_transform,
                                              0.0f,
                                              0.0f,
                                              0.0f);
    REQUIRE(child_origin.x == 10.0f);
    REQUIRE(child_origin.y == 2.0f);
    REQUIRE(child_origin.z == 0.0f);
}

TEST_CASE("Scene graph traversal preserves matrix node transforms",
          "[scene3d][scene]") {
    auto scene = make_textured_triangle_scene();
    scene.root_nodes.clear();
    scene.nodes.clear();

    NodeData root;
    root.name = "matrix-root";
    root.has_matrix_transform = true;
    root.matrix[0] = 2.0f;
    root.matrix[5] = 3.0f;
    root.matrix[10] = 4.0f;
    root.matrix[12] = 10.0f;
    root.children.push_back(1);
    scene.nodes.push_back(std::move(root));

    NodeData child;
    child.name = "matrix-child";
    child.mesh = 0;
    child.has_matrix_transform = true;
    child.matrix[13] = 2.0f;
    scene.nodes.push_back(std::move(child));
    scene.root_nodes.push_back(0);

    std::vector<Diagnostic> diagnostics;
    const auto renderables = collect_renderable_nodes(scene, &diagnostics);

    REQUIRE_FALSE(has_error_diagnostics(diagnostics));
    REQUIRE(renderables.size() == 1);
    REQUIRE(renderables[0].node == 1);
    REQUIRE(renderables[0].mesh == 0);

    const auto point = transform_point(renderables[0].world_transform,
                                       1.0f,
                                       1.0f,
                                       1.0f);
    REQUIRE(point.x == 12.0f);
    REQUIRE(point.y == 9.0f);
    REQUIRE(point.z == 4.0f);
}

TEST_CASE("Scene graph traversal reports node cycles",
          "[scene3d][scene]") {
    SceneData scene;
    NodeData root;
    root.children.push_back(1);
    scene.nodes.push_back(std::move(root));
    NodeData child;
    child.children.push_back(0);
    scene.nodes.push_back(std::move(child));
    scene.root_nodes.push_back(0);

    std::vector<Diagnostic> diagnostics;
    const auto renderables = collect_renderable_nodes(scene, &diagnostics);

    REQUIRE(renderables.empty());
    REQUIRE(has_error_diagnostics(diagnostics));
    REQUIRE(has_code(diagnostics, "scene.graph_cycle"));
}

TEST_CASE("MaterialKey derives renderer feature masks from primitive and material data",
          "[scene3d][material]") {
    auto scene = make_textured_triangle_scene();
    scene.materials[0].double_sided = true;
    scene.materials[0].unlit = true;
    scene.materials[0].base_color_factor[3] = 0.5f;
    scene.materials[0].metallic_roughness_texture = 1;
    scene.materials[0].normal_texture = 2;
    scene.materials[0].occlusion_texture = 3;
    scene.materials[0].emissive_texture = 4;
    scene.materials[0].base_color_transform.enabled = true;
    scene.materials[0].metallic_roughness_transform.enabled = true;
    scene.materials[0].normal_transform.enabled = true;
    scene.materials[0].metallic_roughness_texcoord = 1;
    scene.materials[0].normal_scale = 0.6f;
    scene.materials[0].occlusion_strength = 0.4f;
    scene.materials[0].alpha_mode = MaterialData::AlphaMode::mask;
    for (int i = 0; i < 4; ++i) {
        TextureData texture;
        texture.name = "pbr" + std::to_string(i);
        texture.mime_type = "image/png";
        texture.encoded_bytes = {0x89, 0x50, 0x4e, 0x47};
        scene.textures.push_back(std::move(texture));
    }

    const auto& primitive = scene.meshes[0].primitives[0];
    const auto key = derive_material_key(scene, primitive);

    REQUIRE(key.material == 0);
    REQUIRE(key.has(MaterialFeature::normals));
    REQUIRE(key.has(MaterialFeature::tangents));
    REQUIRE(key.has(MaterialFeature::texcoord0));
    REQUIRE(key.has(MaterialFeature::texcoord1));
    REQUIRE(key.has(MaterialFeature::color0));
    REQUIRE(key.has(MaterialFeature::indexed));
    REQUIRE(key.has(MaterialFeature::base_color_texture));
    REQUIRE(key.has(MaterialFeature::unlit));
    REQUIRE(key.has(MaterialFeature::double_sided));
    REQUIRE(key.has(MaterialFeature::alpha_blend));
    REQUIRE(key.has(MaterialFeature::alpha_mask));
    REQUIRE(key.has(MaterialFeature::metallic_roughness_texture));
    REQUIRE(key.has(MaterialFeature::normal_texture));
    REQUIRE(key.has(MaterialFeature::occlusion_texture));
    REQUIRE(key.has(MaterialFeature::emissive_texture));
    REQUIRE(key.has(MaterialFeature::base_color_texture_transform));
    REQUIRE(key.has(MaterialFeature::non_base_color_texture_transform));
    REQUIRE(key.has(MaterialFeature::non_base_color_texcoord1));
    REQUIRE(key.has(MaterialFeature::normal_scale));
    REQUIRE(key.has(MaterialFeature::occlusion_strength));
    REQUIRE_FALSE(key.has(MaterialFeature::material_fallback));

    const auto names = material_feature_names(key);
    REQUIRE(names.size() == 20);
    REQUIRE(std::string(names[0]) == "normals");
    REQUIRE(std::string(names[1]) == "texcoord0");
    REQUIRE(std::string(names[2]) == "indexed");
    REQUIRE(std::string(names[3]) == "base_color_texture");
    REQUIRE(std::string(names[19]) == "occlusion_strength");
}

TEST_CASE("MaterialKey marks fallback and ignores invalid texture references",
          "[scene3d][material]") {
    auto scene = make_textured_triangle_scene();
    auto primitive = scene.meshes[0].primitives[0];

    primitive.material = invalid_scene_index;
    const auto fallback_key = derive_material_key(scene, primitive);
    REQUIRE(fallback_key.material == invalid_scene_index);
    REQUIRE(fallback_key.has(MaterialFeature::material_fallback));
    REQUIRE(fallback_key.has(MaterialFeature::texcoord0));
    REQUIRE_FALSE(fallback_key.has(MaterialFeature::base_color_texture));

    primitive.material = 0;
    scene.materials[0].base_color_texture = 42;
    scene.materials[0].normal_texture = 43;
    scene.materials[0].base_color_transform.enabled = true;
    scene.materials[0].normal_transform.enabled = true;
    scene.materials[0].normal_texcoord = 1;
    scene.materials[0].normal_scale = 0.5f;
    const auto invalid_texture_key = derive_material_key(scene, primitive);
    REQUIRE(invalid_texture_key.material == 0);
    REQUIRE_FALSE(invalid_texture_key.has(MaterialFeature::material_fallback));
    REQUIRE_FALSE(invalid_texture_key.has(MaterialFeature::base_color_texture));
    REQUIRE_FALSE(invalid_texture_key.has(MaterialFeature::normal_texture));
    REQUIRE_FALSE(invalid_texture_key.has(
        MaterialFeature::base_color_texture_transform));
    REQUIRE_FALSE(invalid_texture_key.has(
        MaterialFeature::non_base_color_texture_transform));
    REQUIRE_FALSE(invalid_texture_key.has(
        MaterialFeature::non_base_color_texcoord1));
    REQUIRE_FALSE(invalid_texture_key.has(MaterialFeature::normal_scale));
}

TEST_CASE("RenderPacket binds node transforms, primitive indices, and material keys",
          "[scene3d][render_packet]") {
    SceneData scene = make_textured_triangle_scene();
    scene.nodes.clear();
    scene.root_nodes.clear();

    MaterialData fallback_material;
    fallback_material.name = "fallback";
    scene.materials.push_back(std::move(fallback_material));

    auto second_primitive = scene.meshes[0].primitives[0];
    second_primitive.material = 1;
    second_primitive.texcoord0.clear();
    scene.meshes[0].primitives.push_back(std::move(second_primitive));

    NodeData root;
    root.name = "root";
    root.translation[0] = 4.0f;
    root.children.push_back(1);
    scene.nodes.push_back(std::move(root));

    NodeData child;
    child.name = "mesh-node";
    child.mesh = 0;
    child.translation[1] = 5.0f;
    scene.nodes.push_back(std::move(child));
    scene.root_nodes.push_back(0);

    const auto packet = build_render_packet(scene);

    REQUIRE_FALSE(packet.has_errors());
    REQUIRE(packet.transformed_nodes.size() == 2);
    REQUIRE(packet.primitives.size() == 2);

    REQUIRE(packet.primitives[0].node == 1);
    REQUIRE(packet.primitives[0].mesh == 0);
    REQUIRE(packet.primitives[0].primitive == 0);
    REQUIRE(packet.primitives[0].material_key.material == 0);
    REQUIRE(packet.primitives[0].material_key.has(
        MaterialFeature::base_color_texture));
    REQUIRE(packet.primitives[0].material_key.has(MaterialFeature::texcoord0));

    REQUIRE(packet.primitives[1].node == 1);
    REQUIRE(packet.primitives[1].mesh == 0);
    REQUIRE(packet.primitives[1].primitive == 1);
    REQUIRE(packet.primitives[1].material_key.material == 1);
    REQUIRE_FALSE(packet.primitives[1].material_key.has(
        MaterialFeature::base_color_texture));
    REQUIRE_FALSE(packet.primitives[1].material_key.has(
        MaterialFeature::texcoord0));

    const auto point = transform_point(packet.primitives[0].world_transform,
                                       1.0f,
                                       1.0f,
                                       0.0f);
    REQUIRE(point.x == 5.0f);
    REQUIRE(point.y == 6.0f);
    REQUIRE(point.z == 0.0f);
}

TEST_CASE("RenderPacket reports graph errors before producing primitives",
          "[scene3d][render_packet]") {
    SceneData scene = make_textured_triangle_scene();
    scene.nodes[0].children.push_back(0);

    const auto packet = build_render_packet(scene);

    REQUIRE(packet.has_errors());
    REQUIRE(packet.primitives.empty());
    REQUIRE(has_code(packet.diagnostics, "scene.graph_cycle"));
}

TEST_CASE("SidecarData carries provenance, unsupported features, and diagnostics",
          "[scene3d][sidecar]") {
    SidecarData sidecar;
    sidecar.provenance.source = "live-threejs";
    sidecar.provenance.exporter = "GLTFExporter";
    sidecar.provenance.exported_at = "2026-06-03T00:00:00Z";
    sidecar.provenance.runtime_evidence =
        "https://github.com/Generous-Corp/pulp/issues/3369";

    sidecar.unsupported_features.push_back(UnsupportedFeature{
        "ShaderMaterial",
        "No glTF metallic-roughness representation.",
        "/Scene/Mesh",
    });
    sidecar.runtime_hints.push_back(RuntimeHint{"preferredCamera", "Camera"});
    append_diagnostic(sidecar.diagnostics,
                      Diagnostic::Severity::warning,
                      "scene.unsupported_material",
                      "ShaderMaterial is live-only.");

    REQUIRE(sidecar.provenance.exporter == "GLTFExporter");
    REQUIRE(sidecar.provenance.runtime_evidence.find("issues/3369") !=
            std::string::npos);
    REQUIRE(sidecar.unsupported_features.size() == 1);
    REQUIRE(sidecar.runtime_hints[0].key == "preferredCamera");
    REQUIRE_FALSE(has_error_diagnostics(sidecar.diagnostics));
}

TEST_CASE("SidecarData parser requires non-empty core provenance fields",
          "[scene3d][sidecar]") {
    const auto valid_without_runtime_evidence =
        sidecar_from_json(make_minimal_sidecar_json("fixture.glb",
                                                   "pulp-scene3d-sidecar",
                                                   "2026-06-03T00:00:00Z",
                                                   ""));
    INFO(valid_without_runtime_evidence.error);
    REQUIRE(valid_without_runtime_evidence.success);
    REQUIRE(valid_without_runtime_evidence.sidecar.provenance.source ==
            "fixture.glb");
    REQUIRE(valid_without_runtime_evidence.sidecar.provenance.runtime_evidence.empty());

    for (const auto& [name, json] : {
             std::pair{"empty-source",
                       make_minimal_sidecar_json("",
                                                 "pulp-scene3d-sidecar",
                                                 "2026-06-03T00:00:00Z",
                                                 "")},
             std::pair{"empty-exporter",
                       make_minimal_sidecar_json("fixture.glb",
                                                 "",
                                                 "2026-06-03T00:00:00Z",
                                                 "")},
             std::pair{"empty-exported-at",
                       make_minimal_sidecar_json("fixture.glb",
                                                 "pulp-scene3d-sidecar",
                                                 "",
                                                 "")},
         }) {
        const auto parsed = sidecar_from_json(json);
        INFO(name);
        REQUIRE_FALSE(parsed.success);
        REQUIRE(parsed.error == "Sidecar JSON contains an invalid field shape.");
    }
}

TEST_CASE("SidecarData analyzer reports native bake/runtime gaps from SceneData",
          "[scene3d][sidecar]") {
    SceneData scene = make_textured_triangle_scene();
    scene.meshes[0].primitives[0].tangents.clear();
    scene.meshes[0].primitives[0].texcoord1 = {
        0.5f, 0.5f,
        0.5f, 0.5f,
        0.5f, 0.5f,
    };
    scene.diagnostics.push_back(Diagnostic{
        Diagnostic::Severity::warning,
        "gltf.animation_path_unsupported",
        "Animation channel path is outside the first native TRS animation slice.",
        "fixture.glb",
    });
    scene.textures[0].name = "basis-texture";
    scene.textures[0].mime_type = "image/ktx2";
    scene.textures[0].encoded_bytes.clear();
    for (const char* name : {
             "metalrough-texture",
             "normal-texture",
             "occlusion-texture",
             "emissive-texture",
         }) {
        TextureData texture;
        texture.name = name;
        texture.mime_type = "image/png";
        texture.encoded_bytes = {0x89, 0x50, 0x4e, 0x47};
        scene.textures.push_back(std::move(texture));
    }
    scene.materials[0].metallic_roughness_texture = 1;
    scene.materials[0].normal_texture = 2;
    scene.materials[0].occlusion_texture = 3;
    scene.materials[0].emissive_texture = 4;
    scene.materials[0].metallic_roughness_texcoord = 1;
    scene.materials[0].normal_texcoord = 1;
    scene.materials[0].metallic_roughness_transform.enabled = true;
    scene.materials[0].metallic_roughness_transform.offset[0] = 0.25f;
    scene.materials[0].normal_transform.enabled = true;
    scene.materials[0].normal_transform.rotation = 0.125f;
    scene.materials[0].occlusion_texcoord = 1;
    scene.materials[0].normal_scale = 0.6f;
    scene.materials[0].occlusion_strength = 0.4f;
    scene.materials[0].advanced_material_extensions.push_back("KHR_materials_clearcoat");
    scene.materials[0].advanced_material_extensions.push_back("KHR_materials_transmission");

    CameraData camera;
    camera.name = "MainCamera";
    scene.cameras.push_back(std::move(camera));
    scene.nodes[0].camera = 0;

    LightData light;
    light.name = "Key";
    scene.lights.push_back(std::move(light));
    scene.nodes[0].light = 0;

    AnimationSamplerData sampler;
    sampler.input_times = {0.0f, 1.0f};
    sampler.output_components = 3;
    sampler.output_values = {0.0f, 0.0f, 0.0f, 1.0f, 2.0f, 3.0f};
    AnimationData animation;
    animation.name = "Move";
    animation.samplers.push_back(std::move(sampler));
    animation.channels.push_back(AnimationChannelData{
        0,
        0,
        AnimationChannelData::Path::translation,
    });
    scene.animations.push_back(std::move(animation));

    SidecarBuildOptions options;
    options.provenance.source = "live-threejs";
    options.provenance.exporter = "GLTFExporter";
    options.provenance.exported_at = "2026-06-03T18:00:00Z";
    options.provenance.runtime_evidence =
        "https://github.com/Generous-Corp/pulp/issues/3369";
    options.source_path = "fixture.glb";

    const auto sidecar = build_sidecar_from_scene(scene, options);

    REQUIRE(sidecar.provenance.source == "live-threejs");
    REQUIRE(sidecar.provenance.exporter == "GLTFExporter");
    REQUIRE(sidecar.provenance.exported_at == "2026-06-03T18:00:00Z");
    REQUIRE(sidecar.provenance.runtime_evidence ==
            "https://github.com/Generous-Corp/pulp/issues/3369");
    REQUIRE(has_code(sidecar.diagnostics, "gltf.animation_path_unsupported"));
    REQUIRE_FALSE(has_error_diagnostics(sidecar.diagnostics));

    REQUIRE(sidecar.unsupported_features.size() == 7);
    REQUIRE(sidecar.unsupported_features[0].feature == "TexturePayload");
    REQUIRE(sidecar.unsupported_features[0].node_path == "basis-texture");
    REQUIRE(sidecar.unsupported_features[1].feature == "TextureFormat:image/ktx2");
    REQUIRE(sidecar.unsupported_features[1].node_path == "basis-texture");
    REQUIRE(sidecar.unsupported_features[2].feature == "TransformAnimation");
    REQUIRE(sidecar.unsupported_features[2].node_path == "Move");
    REQUIRE(sidecar.unsupported_features[3].feature ==
            "MaterialTexture:normalTangents");
    REQUIRE(sidecar.unsupported_features[3].node_path == "mat");
    REQUIRE(sidecar.unsupported_features[4].feature ==
            "MaterialTexture:normalScale");
    REQUIRE(sidecar.unsupported_features[4].node_path == "mat");
    REQUIRE(sidecar.unsupported_features[5].feature ==
            "MaterialExtension:KHR_materials_clearcoat");
    REQUIRE(sidecar.unsupported_features[5].node_path == "mat");
    REQUIRE(sidecar.unsupported_features[6].feature ==
            "MaterialExtension:KHR_materials_transmission");
    REQUIRE(sidecar.unsupported_features[6].node_path == "mat");

    REQUIRE(sidecar.runtime_hints.size() == 3);
    REQUIRE(sidecar.runtime_hints[0].key == "preferredCamera");
    REQUIRE(sidecar.runtime_hints[0].value == "root");
    REQUIRE(sidecar.runtime_hints[1].key == "lightCount");
    REQUIRE(sidecar.runtime_hints[1].value == "1");
    REQUIRE(sidecar.runtime_hints[2].key == "animationCount");
    REQUIRE(sidecar.runtime_hints[2].value == "1");

    const auto parsed = sidecar_from_json(sidecar_to_json(sidecar));
    INFO(parsed.error);
    REQUIRE(parsed.success);
    REQUIRE(parsed.sidecar.runtime_hints.size() == 3);
    REQUIRE(parsed.sidecar.runtime_hints[0].key == "preferredCamera");
    REQUIRE(parsed.sidecar.runtime_hints[0].value == "root");
    REQUIRE(parsed.sidecar.runtime_hints[1].key == "lightCount");
    REQUIRE(parsed.sidecar.runtime_hints[1].value == "1");
    REQUIRE(parsed.sidecar.runtime_hints[2].key == "animationCount");
    REQUIRE(parsed.sidecar.runtime_hints[2].value == "1");
}

TEST_CASE("SidecarData analyzer omits default-path PBR texture gaps",
          "[scene3d][sidecar]") {
    SceneData scene = make_textured_triangle_scene();
    scene.meshes[0].primitives[0].tangents.clear();
    for (const char* name : {
             "metalrough-texture",
             "normal-texture",
             "occlusion-texture",
             "emissive-texture",
         }) {
        TextureData texture;
        texture.name = name;
        texture.mime_type = "image/png";
        texture.encoded_bytes = {0x89, 0x50, 0x4e, 0x47};
        scene.textures.push_back(std::move(texture));
    }
    scene.materials[0].metallic_roughness_texture = 1;
    scene.materials[0].normal_texture = 2;
    scene.materials[0].occlusion_texture = 3;
    scene.materials[0].emissive_texture = 4;
    scene.materials[0].normal_scale = 0.5f;
    scene.materials[0].occlusion_strength = 0.25f;
    scene.materials[0].emissive_factor[0] = 0.25f;
    scene.materials[0].emissive_factor[1] = 0.5f;
    scene.materials[0].emissive_factor[2] = 0.75f;

    const auto sidecar = build_sidecar_from_scene(scene);

    REQUIRE(sidecar.unsupported_features.empty());
    REQUIRE_FALSE(has_unsupported_feature(sidecar, "MaterialTexture:normalTangents"));
    REQUIRE_FALSE(has_unsupported_feature(sidecar,
                                          "MaterialTextureTransform:nonBaseColor"));
    REQUIRE_FALSE(has_unsupported_feature(sidecar,
                                          "MaterialTexcoord:nonBaseColor"));
    REQUIRE_FALSE(has_unsupported_feature(sidecar, "MaterialTexture:normalScale"));
    REQUIRE_FALSE(has_unsupported_feature(sidecar,
                                          "MaterialTexture:occlusionStrength"));

    const auto report = analyze_bake_preflight(sidecar);
    REQUIRE_FALSE(report.export_blocked);
    REQUIRE_FALSE(report.texture_encoding_blocked);
    REQUIRE_FALSE(report.native_runtime_has_gaps);
    REQUIRE(std::string(bake_preflight_readiness(report)) == "clean");
}

TEST_CASE("SidecarData analyzer reports deferred punctual light coverage",
          "[scene3d][sidecar][bake]") {
    SceneData scene = make_textured_triangle_scene();

    LightData key_point;
    key_point.name = "KeyPoint";
    key_point.type = LightData::Type::point;
    key_point.range = 5.0f;
    scene.lights.push_back(std::move(key_point));
    scene.nodes[0].light = 0;

    NodeData duplicate_key_node;
    duplicate_key_node.name = "duplicate-key-node";
    duplicate_key_node.light = 0;
    scene.nodes.push_back(std::move(duplicate_key_node));
    scene.root_nodes.push_back(1);

    LightData fill_point;
    fill_point.name = "FillPoint";
    fill_point.type = LightData::Type::point;
    fill_point.range = 3.0f;
    scene.lights.push_back(std::move(fill_point));

    NodeData fill_node;
    fill_node.name = "fill-light-node";
    fill_node.light = 1;
    scene.nodes.push_back(std::move(fill_node));
    scene.root_nodes.push_back(2);

    LightData loose_spot;
    loose_spot.name = "LooseSpot";
    loose_spot.type = LightData::Type::spot;
    loose_spot.range = 4.0f;
    scene.lights.push_back(std::move(loose_spot));

    const auto sidecar = build_sidecar_from_scene(scene);

    REQUIRE(sidecar.unsupported_features.size() == 3);
    REQUIRE(sidecar.unsupported_features[0].feature ==
            "PunctualLight:additionalPoint");
    REQUIRE(sidecar.unsupported_features[0].node_path == "duplicate-key-node");
    REQUIRE(sidecar.unsupported_features[1].feature ==
            "PunctualLight:additionalPoint");
    REQUIRE(sidecar.unsupported_features[1].node_path == "FillPoint");
    REQUIRE(sidecar.unsupported_features[2].feature ==
            "PunctualLight:unattachedSpot");
    REQUIRE(sidecar.unsupported_features[2].node_path == "LooseSpot");
    REQUIRE(sidecar.runtime_hints.size() == 1);
    REQUIRE(sidecar.runtime_hints[0].key == "lightCount");
    REQUIRE(sidecar.runtime_hints[0].value == "3");

    const auto report = analyze_bake_preflight(sidecar);
    REQUIRE_FALSE(report.export_blocked);
    REQUIRE(report.native_runtime_has_gaps);
    REQUIRE(report.native_runtime_gaps.size() == 3);
    REQUIRE(report.native_runtime_gaps[0].feature ==
            "PunctualLight:additionalPoint");
    REQUIRE(report.native_runtime_gaps[1].feature ==
            "PunctualLight:additionalPoint");
    REQUIRE(report.native_runtime_gaps[2].feature ==
            "PunctualLight:unattachedSpot");
    REQUIRE(std::string(bake_preflight_readiness(report)) == "native_gaps");
}

TEST_CASE("SidecarData analyzer reports deferred directional light coverage",
          "[scene3d][sidecar][bake]") {
    SceneData scene = make_textured_triangle_scene();

    LightData key_sun;
    key_sun.name = "KeySun";
    key_sun.type = LightData::Type::directional;
    scene.lights.push_back(std::move(key_sun));
    scene.nodes[0].light = 0;

    NodeData duplicate_sun_node;
    duplicate_sun_node.name = "duplicate-sun-node";
    duplicate_sun_node.light = 0;
    scene.nodes.push_back(std::move(duplicate_sun_node));
    scene.root_nodes.push_back(1);

    LightData fill_sun;
    fill_sun.name = "FillSun";
    fill_sun.type = LightData::Type::directional;
    scene.lights.push_back(std::move(fill_sun));

    NodeData fill_node;
    fill_node.name = "fill-sun-node";
    fill_node.light = 1;
    scene.nodes.push_back(std::move(fill_node));
    scene.root_nodes.push_back(2);

    LightData loose_sun;
    loose_sun.name = "LooseSun";
    loose_sun.type = LightData::Type::directional;
    scene.lights.push_back(std::move(loose_sun));

    const auto sidecar = build_sidecar_from_scene(scene);

    REQUIRE(sidecar.unsupported_features.size() == 3);
    REQUIRE(sidecar.unsupported_features[0].feature ==
            "PunctualLight:additionalDirectional");
    REQUIRE(sidecar.unsupported_features[0].node_path ==
            "duplicate-sun-node");
    REQUIRE(sidecar.unsupported_features[1].feature ==
            "PunctualLight:additionalDirectional");
    REQUIRE(sidecar.unsupported_features[1].node_path == "FillSun");
    REQUIRE(sidecar.unsupported_features[2].feature ==
            "PunctualLight:unattachedDirectional");
    REQUIRE(sidecar.unsupported_features[2].node_path == "LooseSun");
    REQUIRE(sidecar.runtime_hints.size() == 1);
    REQUIRE(sidecar.runtime_hints[0].key == "lightCount");
    REQUIRE(sidecar.runtime_hints[0].value == "3");

    const auto report = analyze_bake_preflight(sidecar);
    REQUIRE_FALSE(report.export_blocked);
    REQUIRE(report.native_runtime_has_gaps);
    REQUIRE(report.native_runtime_gaps.size() == 3);
    REQUIRE(report.native_runtime_gaps[0].feature ==
            "PunctualLight:additionalDirectional");
    REQUIRE(report.native_runtime_gaps[1].feature ==
            "PunctualLight:additionalDirectional");
    REQUIRE(report.native_runtime_gaps[2].feature ==
            "PunctualLight:unattachedDirectional");
    REQUIRE(std::string(bake_preflight_readiness(report)) == "native_gaps");
}

TEST_CASE("SidecarData analyzer treats first unattached directional light as fallback",
          "[scene3d][sidecar][bake]") {
    SceneData scene = make_textured_triangle_scene();

    LightData key_sun;
    key_sun.name = "KeySun";
    key_sun.type = LightData::Type::directional;
    scene.lights.push_back(std::move(key_sun));

    LightData fill_sun;
    fill_sun.name = "FillSun";
    fill_sun.type = LightData::Type::directional;
    scene.lights.push_back(std::move(fill_sun));

    const auto sidecar = build_sidecar_from_scene(scene);

    REQUIRE(sidecar.unsupported_features.size() == 1);
    REQUIRE(sidecar.unsupported_features[0].feature ==
            "PunctualLight:additionalDirectional");
    REQUIRE(sidecar.unsupported_features[0].node_path == "FillSun");
    REQUIRE(sidecar.runtime_hints.size() == 1);
    REQUIRE(sidecar.runtime_hints[0].key == "lightCount");
    REQUIRE(sidecar.runtime_hints[0].value == "2");

    const auto report = analyze_bake_preflight(sidecar);
    REQUIRE_FALSE(report.export_blocked);
    REQUIRE(report.native_runtime_has_gaps);
    REQUIRE(report.native_runtime_gaps.size() == 1);
    REQUIRE(report.native_runtime_gaps[0].feature ==
            "PunctualLight:additionalDirectional");
    REQUIRE(std::string(bake_preflight_readiness(report)) == "native_gaps");
}

TEST_CASE("SidecarData analyzer reports deferred camera coverage",
          "[scene3d][sidecar][bake]") {
    SceneData scene = make_textured_triangle_scene();

    CameraData main_camera;
    main_camera.name = "MainCamera";
    main_camera.projection = CameraData::Projection::perspective;
    main_camera.znear = 0.2f;
    main_camera.zfar = 40.0f;
    scene.cameras.push_back(std::move(main_camera));

    CameraData ortho_camera;
    ortho_camera.name = "OrthoCamera";
    ortho_camera.projection = CameraData::Projection::orthographic;
    ortho_camera.ymag = 2.0f;
    scene.cameras.push_back(std::move(ortho_camera));

    scene.nodes[0].camera = 0;
    scene.nodes[0].scale[0] = 2.0f;

    NodeData duplicate_main_node;
    duplicate_main_node.name = "duplicate-main-camera-node";
    duplicate_main_node.camera = 0;
    scene.nodes.push_back(std::move(duplicate_main_node));
    scene.root_nodes.push_back(1);

    NodeData ortho_node;
    ortho_node.name = "ortho-camera-node";
    ortho_node.camera = 1;
    scene.nodes.push_back(std::move(ortho_node));
    scene.root_nodes.push_back(2);

    const auto sidecar = build_sidecar_from_scene(scene);

    REQUIRE(sidecar.unsupported_features.size() == 3);
    REQUIRE(sidecar.unsupported_features[0].feature == "Camera:nodeTransform");
    REQUIRE(sidecar.unsupported_features[0].node_path == "root");
    REQUIRE(sidecar.unsupported_features[1].feature ==
            "Camera:additionalPerspective");
    REQUIRE(sidecar.unsupported_features[1].node_path ==
            "duplicate-main-camera-node");
    REQUIRE(sidecar.unsupported_features[2].feature ==
            "Camera:additionalOrthographic");
    REQUIRE(sidecar.unsupported_features[2].node_path == "OrthoCamera");
    REQUIRE(sidecar.runtime_hints.size() == 1);
    REQUIRE(sidecar.runtime_hints[0].key == "preferredCamera");
    REQUIRE(sidecar.runtime_hints[0].value == "root");

    const auto report = analyze_bake_preflight(sidecar);
    REQUIRE_FALSE(report.export_blocked);
    REQUIRE(report.native_runtime_has_gaps);
    REQUIRE(report.native_runtime_gaps.size() == 3);
    REQUIRE(report.native_runtime_gaps[0].feature == "Camera:nodeTransform");
    REQUIRE(report.native_runtime_gaps[1].feature ==
            "Camera:additionalPerspective");
    REQUIRE(report.native_runtime_gaps[2].feature ==
            "Camera:additionalOrthographic");
    REQUIRE(std::string(bake_preflight_readiness(report)) == "native_gaps");
}

TEST_CASE("Bake diagnostics record known non-bakeable Three.js features",
          "[scene3d][sidecar][bake]") {
    SidecarData sidecar;
    sidecar.provenance.source = "live-threejs";
    sidecar.provenance.exporter = "GLTFExporter";
    sidecar.provenance.exported_at = "2026-06-03T00:00:00Z";
    sidecar.provenance.runtime_evidence =
        "https://github.com/Generous-Corp/pulp/issues/3369#issuecomment-4610177927";

    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::shader_material,
                                    "/Scene/Lead",
                                    "live-scene.js");
    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::raw_shader_material,
                                    "/Scene/Background",
                                    "live-scene.js");
    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::postprocessing,
                                    "/Scene/Composer",
                                    "live-scene.js");
    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::render_target,
                                    "/Scene/Reflector",
                                    "live-scene.js");
    append_bake_unsupported_feature(
        sidecar,
        BakeUnsupportedFeature::texture_encoding_missing,
        "/Scene/TexturedBox/material.map",
        "live-scene.js");
    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::arbitrary_js_animation,
                                    "/Scene/useFrame",
                                    "live-scene.js");
    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::physics,
                                    "/Scene/RigidBodies",
                                    "live-scene.js");
    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::event_handler,
                                    "/Scene/InteractiveMesh/onClick",
                                    "live-scene.js");

    REQUIRE(sidecar.unsupported_features.size() == 8);
    REQUIRE(sidecar.unsupported_features[0].feature == "ShaderMaterial");
    REQUIRE(sidecar.unsupported_features[0].node_path == "/Scene/Lead");
    REQUIRE(sidecar.unsupported_features[1].feature == "RawShaderMaterial");
    REQUIRE(sidecar.unsupported_features[2].feature == "Postprocessing");
    REQUIRE(sidecar.unsupported_features[3].feature == "RenderTarget");
    REQUIRE(sidecar.unsupported_features[4].feature == "TextureEncoding");
    REQUIRE(sidecar.unsupported_features[5].feature == "ArbitraryJSAnimation");
    REQUIRE(sidecar.unsupported_features[6].feature == "Physics");
    REQUIRE(sidecar.unsupported_features[7].feature == "EventHandler");

    REQUIRE(sidecar.diagnostics.size() == 8);
    REQUIRE_FALSE(has_error_diagnostics(sidecar.diagnostics));
    REQUIRE(has_code(sidecar.diagnostics,
                     "bake.unsupported_shader_material"));
    REQUIRE(has_code(sidecar.diagnostics,
                     "bake.unsupported_raw_shader_material"));
    REQUIRE(has_code(sidecar.diagnostics,
                     "bake.unsupported_postprocessing"));
    REQUIRE(has_code(sidecar.diagnostics,
                     "bake.unsupported_render_target"));
    REQUIRE(has_code(sidecar.diagnostics,
                     "bake.texture_encoding_missing"));
    REQUIRE(has_code(sidecar.diagnostics,
                     "bake.unsupported_arbitrary_js_animation"));
    REQUIRE(has_code(sidecar.diagnostics, "bake.unsupported_physics"));
    REQUIRE(has_code(sidecar.diagnostics,
                     "bake.unsupported_event_handler"));
    REQUIRE(sidecar.diagnostics[0].source_path == "live-scene.js");

    const auto json = sidecar_to_json(sidecar);
    REQUIRE(json.find("\"path\": \"live-scene.js\"") != std::string::npos);
    REQUIRE(json.find("source_path") == std::string::npos);

    const auto parsed = sidecar_from_json(json);
    INFO(parsed.error);
    REQUIRE(parsed.success);
    REQUIRE(parsed.sidecar.provenance.runtime_evidence ==
            sidecar.provenance.runtime_evidence);
    REQUIRE(parsed.sidecar.unsupported_features.size() == 8);
    REQUIRE(parsed.sidecar.unsupported_features[4].reason.find("toDataURL") !=
            std::string::npos);
    REQUIRE(parsed.sidecar.diagnostics.size() == 8);
    REQUIRE(has_code(parsed.sidecar.diagnostics,
                     "bake.texture_encoding_missing"));
    REQUIRE(parsed.sidecar.diagnostics[0].source_path == "live-scene.js");
}

TEST_CASE("SidecarData parser accepts legacy diagnostic source_path key",
          "[scene3d][sidecar]") {
    auto parsed = sidecar_from_json(
        R"({
            "schema_version": 1,
            "provenance": {
                "source": "legacy",
                "exporter": "legacy-exporter",
                "exported_at": "2026-06-03T00:00:00Z",
                "runtime_evidence": ""
            },
            "diagnostics": [
                {
                    "severity": "warning",
                    "code": "legacy.path",
                    "message": "legacy path key",
                    "source_path": "legacy-sidecar.json"
                }
            ],
            "unsupported_features": [],
            "runtime_hints": []
        })");

    INFO(parsed.error);
    REQUIRE(parsed.success);
    REQUIRE(parsed.sidecar.diagnostics.size() == 1);
    REQUIRE(parsed.sidecar.diagnostics[0].source_path == "legacy-sidecar.json");
}

TEST_CASE("SidecarData parser rejects value-type and key drift",
          "[scene3d][sidecar]") {
    const char* numeric_provenance = R"({
        "schema_version": 1,
        "provenance": {
            "source": 42,
            "exporter": "scene3d-test",
            "exported_at": "2026-06-03T00:00:00Z",
            "runtime_evidence": ""
        },
        "diagnostics": [],
        "unsupported_features": [],
        "runtime_hints": []
    })";
    auto parsed_numeric = sidecar_from_json(numeric_provenance);
    REQUIRE_FALSE(parsed_numeric.success);
    REQUIRE_FALSE(parsed_numeric.error.empty());

    const char* invalid_severity = R"({
        "schema_version": 1,
        "provenance": {
            "source": "fixture",
            "exporter": "scene3d-test",
            "exported_at": "2026-06-03T00:00:00Z",
            "runtime_evidence": ""
        },
        "diagnostics": [
            {
                "severity": "notice",
                "code": "scene.notice",
                "message": "unsupported severity",
                "path": "fixture.glb"
            }
        ],
        "unsupported_features": [],
        "runtime_hints": []
    })";
    auto parsed_severity = sidecar_from_json(invalid_severity);
    REQUIRE_FALSE(parsed_severity.success);
    REQUIRE_FALSE(parsed_severity.error.empty());

    const char* extra_diagnostic_key = R"({
        "schema_version": 1,
        "provenance": {
            "source": "fixture",
            "exporter": "scene3d-test",
            "exported_at": "2026-06-03T00:00:00Z",
            "runtime_evidence": ""
        },
        "diagnostics": [
            {
                "severity": "warning",
                "code": "scene.warning",
                "message": "extra key",
                "path": "fixture.glb",
                "extra": "drift"
            }
        ],
        "unsupported_features": [],
        "runtime_hints": []
    })";
    auto parsed_extra_key = sidecar_from_json(extra_diagnostic_key);
    REQUIRE_FALSE(parsed_extra_key.success);
    REQUIRE_FALSE(parsed_extra_key.error.empty());

    const char* missing_runtime_hints = R"({
        "schema_version": 1,
        "provenance": {
            "source": "fixture",
            "exporter": "scene3d-test",
            "exported_at": "2026-06-03T00:00:00Z",
            "runtime_evidence": ""
        },
        "diagnostics": [],
        "unsupported_features": []
    })";
    auto parsed_missing_root_key = sidecar_from_json(missing_runtime_hints);
    REQUIRE_FALSE(parsed_missing_root_key.success);
    REQUIRE_FALSE(parsed_missing_root_key.error.empty());
}

TEST_CASE("Bake preflight classifies export blockers and native runtime gaps",
          "[scene3d][sidecar][bake]") {
    SidecarData sidecar;
    sidecar.provenance.source = "live-threejs";
    sidecar.provenance.exporter = "GLTFExporter";
    sidecar.provenance.runtime_evidence =
        "https://github.com/Generous-Corp/pulp/issues/3369#issuecomment-4610177927";

    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::shader_material,
                                    "/Scene/Lead",
                                    "live-scene.js");
    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::raw_shader_material,
                                    "/Scene/Background",
                                    "live-scene.js");
    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::postprocessing,
                                    "/Scene/Composer",
                                    "live-scene.js");
    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::render_target,
                                    "/Scene/Reflector",
                                    "live-scene.js");
    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::arbitrary_js_animation,
                                    "/Scene/useFrame",
                                    "live-scene.js");
    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::physics,
                                    "/Scene/RigidBodies",
                                    "live-scene.js");
    append_bake_unsupported_feature(sidecar,
                                    BakeUnsupportedFeature::event_handler,
                                    "/Scene/InteractiveMesh/onClick",
                                    "live-scene.js");
    append_bake_unsupported_feature(
        sidecar,
        BakeUnsupportedFeature::texture_encoding_missing,
        "/Scene/TexturedBox/material.map",
        "live-scene.js");
    sidecar.unsupported_features.push_back(UnsupportedFeature{
        "TexturePayload",
        "Texture has no encoded bytes available for native upload.",
        "texture[0]",
    });
    sidecar.unsupported_features.push_back(UnsupportedFeature{
        "TransformAnimation",
        "Animation data is parsed, but native playback is not implemented.",
        "Move",
    });
    sidecar.unsupported_features.push_back(UnsupportedFeature{
        "MaterialTexture:normalScale",
        "Normal texture scale is parsed but deferred natively.",
        "mat",
    });
    sidecar.unsupported_features.push_back(UnsupportedFeature{
        "MaterialTexture:occlusionStrength",
        "Occlusion texture strength is parsed but deferred natively.",
        "mat",
    });
    sidecar.unsupported_features.push_back(UnsupportedFeature{
        "PrimitiveMode:Lines",
        "Line primitives are valid glTF, but deferred by the native renderer floor.",
        "LineMesh",
    });
    sidecar.unsupported_features.push_back(UnsupportedFeature{
        "AnimationPath:weights",
        "Animation weights channels are valid glTF, but deferred by the native runtime floor.",
        "MorphWeightsOnly",
    });
    sidecar.unsupported_features.push_back(UnsupportedFeature{
        "Camera:depthRange",
        "Camera depth range is preserved but deferred by the native runtime floor.",
        "MainCamera",
    });

    const auto report = analyze_bake_preflight(sidecar);

    REQUIRE(report.export_blocked);
    REQUIRE(report.texture_encoding_blocked);
    REQUIRE(report.native_runtime_has_gaps);
    REQUIRE_FALSE(report.has_error_diagnostics);
    REQUIRE(report.export_blockers.size() == 7);
    REQUIRE(report.export_blockers[0].feature == "ShaderMaterial");
    REQUIRE(report.export_blockers[1].feature == "RawShaderMaterial");
    REQUIRE(report.export_blockers[2].feature == "Postprocessing");
    REQUIRE(report.export_blockers[3].feature == "RenderTarget");
    REQUIRE(report.export_blockers[4].feature == "ArbitraryJSAnimation");
    REQUIRE(report.export_blockers[5].feature == "Physics");
    REQUIRE(report.export_blockers[6].feature == "EventHandler");
    REQUIRE(report.texture_encoding_blockers.size() == 1);
    REQUIRE(report.texture_encoding_blockers[0].feature == "TextureEncoding");
    REQUIRE(report.native_runtime_gaps.size() == 7);
    REQUIRE(report.native_runtime_gaps[0].feature == "TexturePayload");
    REQUIRE(report.native_runtime_gaps[1].feature == "TransformAnimation");
    REQUIRE(report.native_runtime_gaps[2].feature ==
            "MaterialTexture:normalScale");
    REQUIRE(report.native_runtime_gaps[3].feature ==
            "MaterialTexture:occlusionStrength");
    REQUIRE(report.native_runtime_gaps[4].feature == "PrimitiveMode:Lines");
    REQUIRE(report.native_runtime_gaps[5].feature == "AnimationPath:weights");
    REQUIRE(report.native_runtime_gaps[6].feature == "Camera:depthRange");
    REQUIRE(report.diagnostics.size() == sidecar.diagnostics.size());
    REQUIRE(std::string(bake_preflight_readiness(report)) == "blocked");

    BakePreflightOptions require_runtime_evidence;
    require_runtime_evidence.require_runtime_evidence = true;
    const auto evidence_report =
        analyze_bake_preflight(sidecar, require_runtime_evidence);
    REQUIRE_FALSE(evidence_report.runtime_evidence_missing);
    REQUIRE_FALSE(evidence_report.runtime_evidence_url_invalid);

    BakePreflightOptions require_runtime_evidence_url;
    require_runtime_evidence_url.require_runtime_evidence_url = true;
    const auto evidence_url_report =
        analyze_bake_preflight(sidecar, require_runtime_evidence_url);
    REQUIRE_FALSE(evidence_url_report.runtime_evidence_missing);
    REQUIRE_FALSE(evidence_url_report.runtime_evidence_url_invalid);
}

TEST_CASE("Bake preflight keeps native renderer gaps separate from export readiness",
          "[scene3d][sidecar][bake]") {
    SidecarData sidecar;
    sidecar.unsupported_features.push_back(UnsupportedFeature{
        "MaterialExtension:KHR_materials_clearcoat",
        "Advanced glTF material extension is parsed for diagnostics.",
        "PhysicalMat",
    });
    sidecar.unsupported_features.push_back(UnsupportedFeature{
        "MorphTargets",
        "Morph target attributes are parsed but not rendered natively.",
        "MorphMesh",
    });

    const auto native_gap_report = analyze_bake_preflight(sidecar);

    REQUIRE_FALSE(native_gap_report.export_blocked);
    REQUIRE_FALSE(native_gap_report.texture_encoding_blocked);
    REQUIRE(native_gap_report.native_runtime_has_gaps);
    REQUIRE(native_gap_report.native_runtime_gaps.size() == 2);
    REQUIRE(std::string(bake_preflight_readiness(native_gap_report)) ==
            "native_gaps");

    SidecarData clean_sidecar;
    append_diagnostic(clean_sidecar.diagnostics,
                      Diagnostic::Severity::warning,
                      "bake.note",
                      "Warning diagnostics do not block export by themselves.");

    const auto clean_report = analyze_bake_preflight(clean_sidecar);

    REQUIRE_FALSE(clean_report.export_blocked);
    REQUIRE_FALSE(clean_report.texture_encoding_blocked);
    REQUIRE_FALSE(clean_report.native_runtime_has_gaps);
    REQUIRE_FALSE(clean_report.has_error_diagnostics);
    REQUIRE_FALSE(clean_report.runtime_evidence_missing);
    REQUIRE(std::string(bake_preflight_readiness(clean_report)) == "clean");

    BakePreflightOptions require_runtime_evidence;
    require_runtime_evidence.require_runtime_evidence = true;
    const auto missing_evidence_report =
        analyze_bake_preflight(clean_sidecar, require_runtime_evidence);

    REQUIRE(missing_evidence_report.export_blocked);
    REQUIRE(missing_evidence_report.runtime_evidence_missing);
    REQUIRE_FALSE(missing_evidence_report.runtime_evidence_url_invalid);
    REQUIRE_FALSE(missing_evidence_report.texture_encoding_blocked);
    REQUIRE_FALSE(missing_evidence_report.native_runtime_has_gaps);
    REQUIRE(std::string(bake_preflight_readiness(missing_evidence_report)) ==
            "blocked");

    clean_sidecar.provenance.runtime_evidence = "native-preflight-fixture";
    BakePreflightOptions require_runtime_evidence_url;
    require_runtime_evidence_url.require_runtime_evidence_url = true;
    const auto invalid_url_report =
        analyze_bake_preflight(clean_sidecar, require_runtime_evidence_url);
    REQUIRE(invalid_url_report.export_blocked);
    REQUIRE_FALSE(invalid_url_report.runtime_evidence_missing);
    REQUIRE(invalid_url_report.runtime_evidence_url_invalid);
    REQUIRE_FALSE(invalid_url_report.texture_encoding_blocked);
    REQUIRE_FALSE(invalid_url_report.native_runtime_has_gaps);
    REQUIRE(std::string(bake_preflight_readiness(invalid_url_report)) ==
            "blocked");

    clean_sidecar.provenance.runtime_evidence =
        "https://github.com/Generous-Corp/pulp/issues/2738#issuecomment-4618508064";
    const auto valid_url_report =
        analyze_bake_preflight(clean_sidecar, require_runtime_evidence_url);
    REQUIRE_FALSE(valid_url_report.export_blocked);
    REQUIRE_FALSE(valid_url_report.runtime_evidence_missing);
    REQUIRE_FALSE(valid_url_report.runtime_evidence_url_invalid);
    REQUIRE(std::string(bake_preflight_readiness(valid_url_report)) == "clean");

    append_diagnostic(clean_sidecar.diagnostics,
                      Diagnostic::Severity::error,
                      "scene.invalid",
                      "Structural errors block bake preflight.");

    const auto error_report = analyze_bake_preflight(clean_sidecar);

    REQUIRE(error_report.export_blocked);
    REQUIRE(error_report.has_error_diagnostics);
}

TEST_CASE("SidecarData serializes diagnostics, unsupported features, and hints",
          "[scene3d][sidecar]") {
    SidecarData sidecar;
    sidecar.provenance.source = "live-threejs";
    sidecar.provenance.exporter = "GLTFExporter";
    sidecar.provenance.exported_at = "2026-06-03T12:34:56Z";
    sidecar.provenance.runtime_evidence =
        "https://github.com/Generous-Corp/pulp/issues/3369#issuecomment-4610232717";

    sidecar.unsupported_features.push_back(UnsupportedFeature{
        "ShaderMaterial",
        "No glTF metallic-roughness representation; stay \"Live\".",
        "/Scene/Mesh \"Lead\"",
    });
    sidecar.runtime_hints.push_back(RuntimeHint{"preferredCamera", "Camera_Main"});
    append_diagnostic(sidecar.diagnostics,
                      Diagnostic::Severity::warning,
                      "scene.unsupported_material",
                      "ShaderMaterial is live-only.",
                      "fixture.glb");
    append_diagnostic(sidecar.diagnostics,
                      Diagnostic::Severity::error,
                      "scene.postprocessing_live_only",
                      "EffectComposer cannot bake.",
                      "fixture.glb");

    const auto json = sidecar_to_json(sidecar);

    REQUIRE(json.find("\"schema_version\": 1") != std::string::npos);
    REQUIRE(json.find("\\\"Live\\\"") != std::string::npos);

    auto parsed = sidecar_from_json(json);

    INFO(parsed.error);
    REQUIRE(parsed.success);
    REQUIRE(parsed.sidecar.provenance.source == sidecar.provenance.source);
    REQUIRE(parsed.sidecar.provenance.exporter == sidecar.provenance.exporter);
    REQUIRE(parsed.sidecar.provenance.exported_at == sidecar.provenance.exported_at);
    REQUIRE(parsed.sidecar.provenance.runtime_evidence ==
            sidecar.provenance.runtime_evidence);
    REQUIRE(parsed.sidecar.unsupported_features.size() == 1);
    REQUIRE(parsed.sidecar.unsupported_features[0].feature == "ShaderMaterial");
    REQUIRE(parsed.sidecar.unsupported_features[0].reason ==
            "No glTF metallic-roughness representation; stay \"Live\".");
    REQUIRE(parsed.sidecar.unsupported_features[0].node_path == "/Scene/Mesh \"Lead\"");
    REQUIRE(parsed.sidecar.runtime_hints.size() == 1);
    REQUIRE(parsed.sidecar.runtime_hints[0].key == "preferredCamera");
    REQUIRE(parsed.sidecar.runtime_hints[0].value == "Camera_Main");
    REQUIRE(parsed.sidecar.diagnostics.size() == 2);
    REQUIRE(parsed.sidecar.diagnostics[0].severity == Diagnostic::Severity::warning);
    REQUIRE(parsed.sidecar.diagnostics[0].code == "scene.unsupported_material");
    REQUIRE(parsed.sidecar.diagnostics[1].severity == Diagnostic::Severity::error);
    REQUIRE(has_error_diagnostics(parsed.sidecar.diagnostics));
}

TEST_CASE("SidecarData parser rejects malformed sidecars",
          "[scene3d][sidecar]") {
    auto missing_version = sidecar_from_json(R"({"provenance":{}})");
    REQUIRE_FALSE(missing_version.success);
    REQUIRE_FALSE(missing_version.error.empty());

    auto wrong_shape = sidecar_from_json(
        R"({"schema_version":1,"diagnostics":{}})");
    REQUIRE_FALSE(wrong_shape.success);
    REQUIRE_FALSE(wrong_shape.error.empty());
}

TEST_CASE("load_gltf_scene maps generated embedded GLB into SceneData",
          "[scene3d][gltf]") {
    auto path = write_temp_file("pulp-scene3d-minimal-textured.glb",
                                make_minimal_textured_glb());

    auto result = load_gltf_scene(path);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(result.scene.root_nodes == std::vector<uint32_t>{0});
    REQUIRE(result.scene.nodes.size() == 1);
    REQUIRE(result.scene.nodes[0].name == "root");
    REQUIRE(result.scene.nodes[0].mesh == 0);
    REQUIRE(result.scene.nodes[0].translation[0] == 1.0f);
    REQUIRE(result.scene.nodes[0].translation[1] == 2.0f);
    REQUIRE(result.scene.nodes[0].translation[2] == 3.0f);
    REQUIRE_FALSE(result.scene.nodes[0].has_matrix_transform);

    REQUIRE(result.scene.meshes.size() == 1);
    REQUIRE(result.scene.meshes[0].name == "triangle");
    REQUIRE(result.scene.meshes[0].primitives.size() == 1);
    const auto& primitive = result.scene.meshes[0].primitives[0];
    REQUIRE(primitive.positions.size() == 9);
    REQUIRE(primitive.normals.size() == 9);
    REQUIRE(primitive.tangents ==
            std::vector<float>{1.0f, 0.0f, 0.0f, 1.0f,
                               1.0f, 0.0f, 0.0f, 1.0f,
                               1.0f, 0.0f, 0.0f, 1.0f});
    REQUIRE(primitive.texcoord0.size() == 6);
    REQUIRE(primitive.texcoord1 ==
            std::vector<float>{0.25f, 0.25f, 0.75f, 0.25f, 0.5f, 0.75f});
    REQUIRE(primitive.color0 ==
            std::vector<float>{1.0f, 0.0f, 0.0f, 1.0f,
                               0.0f, 1.0f, 0.0f, 1.0f,
                               0.0f, 0.0f, 1.0f, 1.0f});
    REQUIRE(primitive.indices == std::vector<uint32_t>{0, 1, 2});
    REQUIRE(primitive.material == 0);

    REQUIRE(result.scene.materials.size() == 1);
    REQUIRE(result.scene.materials[0].name == "mat");
    REQUIRE(result.scene.materials[0].base_color_texture == 0);
    REQUIRE(result.scene.materials[0].base_color_factor[0] == 0.25f);
    REQUIRE(result.scene.materials[0].base_color_factor[1] == 0.5f);
    REQUIRE(result.scene.materials[0].base_color_factor[2] == 0.75f);
    REQUIRE(result.scene.materials[0].base_color_factor[3] == 1.0f);
    REQUIRE(result.scene.materials[0].double_sided);

    REQUIRE(result.scene.textures.size() == 1);
    REQUIRE(result.scene.textures[0].name == "checker");
    REQUIRE(result.scene.textures[0].mime_type == "image/png");
    REQUIRE(result.scene.textures[0].encoded_bytes ==
            std::vector<uint8_t>{0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a});
}

TEST_CASE("load_gltf_scene maps official BoxTextured fixture into SceneData",
          "[scene3d][gltf][fixture]") {
    const auto path = scene3d_fixture_path("BoxTextured/BoxTextured.glb");
    REQUIRE(std::filesystem::exists(path));

    auto result = load_gltf_scene(path);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(result.scene.meshes.size() == 1);
    REQUIRE(result.scene.meshes[0].primitives.size() == 1);

    const auto& primitive = result.scene.meshes[0].primitives[0];
    REQUIRE(primitive.positions.size() == 24u * 3u);
    REQUIRE(primitive.normals.size() == 24u * 3u);
    REQUIRE(primitive.texcoord0.size() == 24u * 2u);
    REQUIRE(primitive.indices.size() == 36u);
    REQUIRE(is_valid_scene_index(primitive.material,
                                 result.scene.materials.size()));

    REQUIRE(result.scene.materials.size() == 1);
    REQUIRE(is_valid_scene_index(result.scene.materials[0].base_color_texture,
                                 result.scene.textures.size()));
    REQUIRE(result.scene.textures.size() == 1);
    REQUIRE(result.scene.textures[0].mime_type == "image/png");
    REQUIRE_FALSE(result.scene.textures[0].encoded_bytes.empty());

    SidecarBuildOptions options;
    options.provenance.source = "Khronos glTF Sample Assets BoxTextured";
    options.provenance.exporter = "upstream glTF-Sample-Assets";
    options.provenance.runtime_evidence =
        "https://github.com/Generous-Corp/pulp/issues/2738";
    options.source_path = path.string();

    const auto sidecar = build_sidecar_from_scene(result.scene, options);
    REQUIRE(sidecar.provenance.runtime_evidence ==
            "https://github.com/Generous-Corp/pulp/issues/2738");
    REQUIRE_FALSE(has_error_diagnostics(sidecar.diagnostics));
}

TEST_CASE("load_gltf_scene stats summarize official BoxTextured fixture",
          "[scene3d][gltf][fixture][stats]") {
    const auto path = scene3d_fixture_path("BoxTextured/BoxTextured.glb");
    REQUIRE(std::filesystem::exists(path));

    auto result = load_gltf_scene(path);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error_diagnostics(result.scene.diagnostics));

    const auto stats = summarize_scene_data(result.scene);
    REQUIRE(stats.meshes == 1);
    REQUIRE(stats.primitives == 1);
    REQUIRE(stats.indexed_primitives == 1);
    REQUIRE(stats.vertices == 24);
    REQUIRE(stats.indices == 36);
    REQUIRE(stats.materials == 1);
    REQUIRE(stats.textures == 1);
    REQUIRE(stats.texture_bytes > 0);
    REQUIRE(stats.error_diagnostics == 0);

    const auto text = scene_stats_to_text(stats);
    REQUIRE(text.find("meshes=1") != std::string::npos);
    REQUIRE(text.find("vertices=24") != std::string::npos);
    REQUIRE(text.find("indices=36") != std::string::npos);
    REQUIRE(text.find("textures=1") != std::string::npos);
}

TEST_CASE("load_gltf_scene preserves matrix node transforms",
          "[scene3d][gltf]") {
    const auto dir = std::filesystem::temp_directory_path() /
                     "pulp-scene3d-matrix-gltf";
    std::filesystem::create_directories(dir);

    const auto bin = make_minimal_external_gltf_bin();
    write_file(dir / "mesh.bin", bin);
    const auto gltf_path = dir / "matrix.gltf";
    write_file(
        gltf_path,
        R"({"asset":{"version":"2.0","generator":"pulp-test-scene3d-matrix"},"buffers":[{"uri":"mesh.bin","byteLength":126}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":36,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":72,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":96,"byteLength":6,"target":34963}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5126,"count":3,"type":"VEC3"},{"bufferView":2,"componentType":5126,"count":3,"type":"VEC2"},{"bufferView":3,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"name":"matrix-triangle","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"mode":4}]}],"nodes":[{"name":"matrix-root","mesh":0,"matrix":[2,0,0,0,0,3,0,0,0,0,4,0,10,20,30,1]}],"scenes":[{"nodes":[0]}],"scene":0})");

    auto result = load_gltf_scene(gltf_path);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(result.scene.nodes.size() == 1);
    const auto& node = result.scene.nodes[0];
    REQUIRE(node.name == "matrix-root");
    REQUIRE(node.mesh == 0);
    REQUIRE(node.has_matrix_transform);
    REQUIRE(node.matrix[0] == 2.0f);
    REQUIRE(node.matrix[5] == 3.0f);
    REQUIRE(node.matrix[10] == 4.0f);
    REQUIRE(node.matrix[12] == 10.0f);
    REQUIRE(node.matrix[13] == 20.0f);
    REQUIRE(node.matrix[14] == 30.0f);

    std::vector<Diagnostic> diagnostics;
    const auto renderables = collect_renderable_nodes(result.scene, &diagnostics);
    REQUIRE_FALSE(has_error_diagnostics(diagnostics));
    REQUIRE(renderables.size() == 1);
    const auto point = transform_point(renderables[0].world_transform,
                                       1.0f,
                                       1.0f,
                                       1.0f);
    REQUIRE(point.x == 12.0f);
    REQUIRE(point.y == 23.0f);
    REQUIRE(point.z == 34.0f);
}

TEST_CASE("load_gltf_scene loads local external glTF buffers and images",
          "[scene3d][gltf]") {
    const auto dir = std::filesystem::temp_directory_path() / "pulp-scene3d-external-gltf";
    std::filesystem::create_directories(dir);

    const auto bin = make_minimal_external_gltf_bin();
    const std::vector<uint8_t> fake_png = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    };
    write_file(dir / "mesh.bin", bin);
    write_file(dir / "checker.png", fake_png);
    const auto gltf_path = dir / "scene.gltf";
    write_file(gltf_path, make_minimal_external_gltf_json(bin.size()));

    auto result = load_gltf_scene(gltf_path);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(result.scene.root_nodes == std::vector<uint32_t>{0});
    REQUIRE(result.scene.nodes.size() == 1);
    REQUIRE(result.scene.nodes[0].name == "external-root");

    REQUIRE(result.scene.meshes.size() == 1);
    REQUIRE(result.scene.meshes[0].name == "external-triangle");
    REQUIRE(result.scene.meshes[0].primitives.size() == 1);
    const auto& primitive = result.scene.meshes[0].primitives[0];
    REQUIRE(primitive.positions.size() == 9);
    REQUIRE(primitive.normals.size() == 9);
    REQUIRE(primitive.texcoord0.size() == 6);
    REQUIRE(primitive.indices == std::vector<uint32_t>{0, 1, 2});
    REQUIRE(primitive.material == 0);

    REQUIRE(result.scene.materials.size() == 1);
    REQUIRE(result.scene.materials[0].name == "external-mat");
    REQUIRE(result.scene.materials[0].base_color_texture == 0);

    REQUIRE(result.scene.textures.size() == 1);
    REQUIRE(result.scene.textures[0].name == "external-checker");
    REQUIRE(result.scene.textures[0].mime_type == "image/png");
    REQUIRE(result.scene.textures[0].encoded_bytes == fake_png);
}

TEST_CASE("load_gltf_scene maps cameras and KHR_lights_punctual into SceneData",
          "[scene3d][gltf]") {
    const auto path = std::filesystem::temp_directory_path() /
        "pulp-scene3d-camera-light.gltf";
    write_file(path, make_camera_light_gltf_json());

    auto result = load_gltf_scene(path);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(result.scene.cameras.size() == 2);
    REQUIRE(result.scene.cameras[0].name == "MainCamera");
    REQUIRE(result.scene.cameras[0].projection == CameraData::Projection::perspective);
    REQUIRE(result.scene.cameras[0].aspect_ratio == 1.5f);
    REQUIRE(result.scene.cameras[0].yfov == 0.75f);
    REQUIRE(result.scene.cameras[0].znear == 0.1f);
    REQUIRE(result.scene.cameras[0].zfar == 50.0f);
    REQUIRE(result.scene.cameras[1].name == "OrthoCamera");
    REQUIRE(result.scene.cameras[1].projection == CameraData::Projection::orthographic);
    REQUIRE(result.scene.cameras[1].xmag == 4.0f);
    REQUIRE(result.scene.cameras[1].ymag == 3.0f);
    REQUIRE(result.scene.cameras[1].znear == 0.2f);
    REQUIRE(result.scene.cameras[1].zfar == 20.0f);

    REQUIRE(result.scene.lights.size() == 3);
    REQUIRE(result.scene.lights[0].name == "Key");
    REQUIRE(result.scene.lights[0].type == LightData::Type::directional);
    REQUIRE(result.scene.lights[0].color[0] == 1.0f);
    REQUIRE(result.scene.lights[0].color[1] == 0.8f);
    REQUIRE(result.scene.lights[0].color[2] == 0.6f);
    REQUIRE(result.scene.lights[0].intensity == 2.5f);
    REQUIRE(result.scene.lights[1].type == LightData::Type::point);
    REQUIRE(result.scene.lights[1].range == 9.0f);
    REQUIRE(result.scene.lights[2].type == LightData::Type::spot);
    REQUIRE(result.scene.lights[2].inner_cone_angle == 0.2f);
    REQUIRE(result.scene.lights[2].outer_cone_angle == 0.7f);

    REQUIRE(result.scene.nodes.size() == 4);
    REQUIRE(result.scene.nodes[0].camera == 0);
    REQUIRE(result.scene.nodes[1].light == 0);
    REQUIRE(result.scene.nodes[2].light == 2);
    REQUIRE(result.scene.nodes[3].camera == 1);
}

TEST_CASE("load_gltf_scene maps PBR material factors and texture slots",
          "[scene3d][gltf]") {
    const auto dir = std::filesystem::temp_directory_path() / "pulp-scene3d-pbr-material";
    std::filesystem::create_directories(dir);

    const std::vector<uint8_t> fake_png = {
        0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a,
    };
    for (const auto& name : {
             "base.png",
             "metalrough.png",
             "normal.png",
             "occlusion.png",
             "emissive.png",
         }) {
        write_file(dir / name, fake_png);
    }
    const auto gltf_path = dir / "material.gltf";
    write_file(gltf_path, make_pbr_material_gltf_json());

    auto result = load_gltf_scene(gltf_path);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(result.scene.textures.size() == 5);
    REQUIRE(result.scene.texture_samplers.size() == 2);
    REQUIRE(result.scene.materials.size() == 1);

    REQUIRE(result.scene.texture_samplers[0].name == "nearestClamp");
    REQUIRE(result.scene.texture_samplers[0].mag_filter ==
            TextureSamplerData::Filter::nearest);
    REQUIRE(result.scene.texture_samplers[0].min_filter ==
            TextureSamplerData::Filter::nearest_mipmap_nearest);
    REQUIRE(result.scene.texture_samplers[0].wrap_s ==
            TextureSamplerData::Wrap::clamp_to_edge);
    REQUIRE(result.scene.texture_samplers[0].wrap_t ==
            TextureSamplerData::Wrap::mirrored_repeat);
    REQUIRE(result.scene.texture_samplers[1].name == "linearRepeat");
    REQUIRE(result.scene.texture_samplers[1].mag_filter ==
            TextureSamplerData::Filter::linear);
    REQUIRE(result.scene.texture_samplers[1].min_filter ==
            TextureSamplerData::Filter::linear_mipmap_linear);
    REQUIRE(result.scene.texture_samplers[1].wrap_s ==
            TextureSamplerData::Wrap::repeat);
    REQUIRE(result.scene.texture_samplers[1].wrap_t ==
            TextureSamplerData::Wrap::repeat);

    const auto& material = result.scene.materials[0];
    REQUIRE(material.name == "PBRMat");
    REQUIRE(material.base_color_texture == 0);
    REQUIRE(material.metallic_roughness_texture == 1);
    REQUIRE(material.normal_texture == 2);
    REQUIRE(material.occlusion_texture == 3);
    REQUIRE(material.emissive_texture == 4);
    REQUIRE(material.base_color_sampler == 0);
    REQUIRE(material.metallic_roughness_sampler == 1);
    REQUIRE(material.normal_sampler == 1);
    REQUIRE(material.occlusion_sampler == 0);
    REQUIRE(material.emissive_sampler == 1);
    REQUIRE(material.base_color_texcoord == 1);
    REQUIRE(material.metallic_roughness_texcoord == 1);
    REQUIRE(material.normal_texcoord == 1);
    REQUIRE(material.occlusion_texcoord == 1);
    REQUIRE(material.emissive_texcoord == 1);
    REQUIRE(material.normal_scale == 0.65f);
    REQUIRE(material.occlusion_strength == 0.4f);
    REQUIRE(material.base_color_transform.enabled);
    REQUIRE(material.base_color_transform.offset[0] == 0.25f);
    REQUIRE(material.base_color_transform.offset[1] == 0.5f);
    REQUIRE(material.base_color_transform.scale[0] == 2.0f);
    REQUIRE(material.base_color_transform.scale[1] == 3.0f);
    REQUIRE(material.base_color_transform.rotation == 0.125f);
    REQUIRE(material.base_color_transform.texcoord_override == 1);
    REQUIRE_FALSE(material.metallic_roughness_transform.enabled);
    REQUIRE(material.base_color_factor[0] == 0.2f);
    REQUIRE(material.base_color_factor[1] == 0.4f);
    REQUIRE(material.base_color_factor[2] == 0.6f);
    REQUIRE(material.base_color_factor[3] == 0.8f);
    REQUIRE(material.metallic_factor == 0.7f);
    REQUIRE(material.roughness_factor == 0.35f);
    REQUIRE(material.emissive_factor[0] == 0.1f);
    REQUIRE(material.emissive_factor[1] == 0.2f);
    REQUIRE(material.emissive_factor[2] == 0.3f);
    REQUIRE(material.alpha_mode == MaterialData::AlphaMode::mask);
    REQUIRE(material.alpha_cutoff == 0.42f);
    REQUIRE(material.double_sided);
}

TEST_CASE("load_gltf_scene records advanced material extensions for sidecar diagnostics",
          "[scene3d][gltf]") {
    const auto path = std::filesystem::temp_directory_path() /
        "pulp-scene3d-advanced-material.gltf";
    write_file(path, make_advanced_material_gltf_json());

    auto result = load_gltf_scene(path);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(has_code(result.scene.diagnostics,
                     "gltf.material_extension_deferred"));
    REQUIRE(result.scene.materials.size() == 1);

    const auto& material = result.scene.materials[0];
    REQUIRE(material.name == "PhysicalMat");
    REQUIRE(material.advanced_material_extensions ==
            std::vector<std::string>{
                "KHR_materials_clearcoat",
                "KHR_materials_transmission",
                "KHR_materials_sheen",
                "KHR_materials_specular",
                "KHR_materials_volume",
                "KHR_materials_anisotropy",
                "KHR_materials_iridescence",
                "KHR_materials_diffuse_transmission",
                "KHR_materials_ior",
                "KHR_materials_dispersion",
            });

    const auto sidecar = build_sidecar_from_scene(result.scene);
    REQUIRE(has_code(sidecar.diagnostics,
                     "gltf.material_extension_deferred"));
    REQUIRE(sidecar.unsupported_features.size() == 10);
    REQUIRE(sidecar.unsupported_features[0].feature ==
            "MaterialExtension:KHR_materials_clearcoat");
    REQUIRE(sidecar.unsupported_features[9].feature ==
            "MaterialExtension:KHR_materials_dispersion");
    REQUIRE(sidecar.unsupported_features[0].node_path == "PhysicalMat");
}

TEST_CASE("load_gltf_scene reports unsupported native mesh features",
          "[scene3d][gltf]") {
    const auto dir = std::filesystem::temp_directory_path() /
        "pulp-scene3d-unsupported-mesh-features";
    std::filesystem::create_directories(dir);

    const auto bin = make_unsupported_mesh_features_gltf_bin();
    write_file(dir / "unsupported.bin", bin);
    const auto gltf_path = dir / "unsupported.gltf";
    write_file(gltf_path, make_unsupported_mesh_features_gltf_json(bin.size()));

    auto result = load_gltf_scene(gltf_path);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(has_code(result.scene.diagnostics, "gltf.mesh_morph_weights_unsupported"));
    REQUIRE(has_code(result.scene.diagnostics, "gltf.primitive_morph_targets_unsupported"));
    REQUIRE(has_code(result.scene.diagnostics, "gltf.node_skin_unsupported"));
    REQUIRE(has_code(result.scene.diagnostics, "gltf.node_instancing_unsupported"));
    REQUIRE(has_code_path(result.scene.diagnostics,
                          "gltf.mesh_morph_weights_unsupported",
                          gltf_path));
    REQUIRE(has_code_path(result.scene.diagnostics,
                          "gltf.primitive_morph_targets_unsupported",
                          gltf_path));
    REQUIRE(has_code_path(result.scene.diagnostics,
                          "gltf.node_skin_unsupported",
                          gltf_path));
    REQUIRE(has_code_path(result.scene.diagnostics,
                          "gltf.node_instancing_unsupported",
                          gltf_path));
    REQUIRE(result.scene.unsupported_features.size() == 4);

    const auto sidecar = build_sidecar_from_scene(result.scene);
    REQUIRE(sidecar.unsupported_features.size() == 4);
    REQUIRE(sidecar.unsupported_features[0].feature == "MorphWeights");
    REQUIRE(sidecar.unsupported_features[0].node_path == "MorphMesh");
    REQUIRE(sidecar.unsupported_features[1].feature == "MorphTargets");
    REQUIRE(sidecar.unsupported_features[1].node_path == "MorphMesh");
    REQUIRE(sidecar.unsupported_features[2].feature == "Skinning");
    REQUIRE(sidecar.unsupported_features[2].node_path == "SkinnedMorphed");
    REQUIRE(sidecar.unsupported_features[3].feature == "GpuInstancing");
    REQUIRE(sidecar.unsupported_features[3].node_path == "InstancedNode");
}

TEST_CASE("load_gltf_scene reports non-triangle primitive modes as native gaps",
          "[scene3d][gltf]") {
    const auto dir = std::filesystem::temp_directory_path() /
        "pulp-scene3d-unsupported-primitive-mode";
    std::filesystem::create_directories(dir);

    std::vector<uint8_t> bin;
    append_f32(bin, 0.0f); append_f32(bin, 0.0f); append_f32(bin, 0.0f);
    append_f32(bin, 1.0f); append_f32(bin, 0.0f); append_f32(bin, 0.0f);
    append_u16(bin, 0); append_u16(bin, 1);
    write_file(dir / "lines.bin", bin);

    const auto gltf_path = dir / "lines.gltf";
    write_file(
        gltf_path,
        R"({"asset":{"version":"2.0","generator":"pulp-test-scene3d-primitive-mode"},"buffers":[{"uri":"lines.bin","byteLength":28}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":24,"target":34962},{"buffer":0,"byteOffset":24,"byteLength":4,"target":34963}],"accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"VEC3","min":[0,0,0],"max":[1,0,0]},{"bufferView":1,"componentType":5123,"count":2,"type":"SCALAR"}],"meshes":[{"name":"LineMesh","primitives":[{"attributes":{"POSITION":0},"indices":1,"mode":1}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");

    auto result = load_gltf_scene(gltf_path);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(has_code_path(result.scene.diagnostics,
                          "gltf.primitive_unsupported_mode",
                          gltf_path));
    REQUIRE(has_code_path(result.scene.diagnostics,
                          "scene.mesh_without_primitives",
                          gltf_path));
    REQUIRE(result.scene.unsupported_features.size() == 1);
    REQUIRE(result.scene.unsupported_features[0].feature ==
            "PrimitiveMode:Lines");
    REQUIRE(result.scene.unsupported_features[0].node_path == "LineMesh");
    REQUIRE(result.scene.meshes.size() == 1);
    REQUIRE(result.scene.meshes[0].primitives.empty());

    const auto sidecar = build_sidecar_from_scene(result.scene);
    REQUIRE(sidecar.unsupported_features.size() == 1);
    REQUIRE(sidecar.unsupported_features[0].feature == "PrimitiveMode:Lines");

    const auto report = analyze_bake_preflight(sidecar);
    REQUIRE_FALSE(report.export_blocked);
    REQUIRE(report.native_runtime_has_gaps);
    REQUIRE(report.native_runtime_gaps.size() == 1);
    REQUIRE(report.native_runtime_gaps[0].feature == "PrimitiveMode:Lines");
    REQUIRE(std::string(bake_preflight_readiness(report)) == "native_gaps");
}

TEST_CASE("load_gltf_scene maps TRS animation channels into SceneData",
          "[scene3d][gltf]") {
    const auto dir = std::filesystem::temp_directory_path() / "pulp-scene3d-animation-gltf";
    std::filesystem::create_directories(dir);

    const auto bin = make_animation_gltf_bin();
    write_file(dir / "anim.bin", bin);
    const auto gltf_path = dir / "animated.gltf";
    write_file(gltf_path, make_animation_gltf_json(bin.size()));

    auto result = load_gltf_scene(gltf_path);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(has_code(result.scene.diagnostics, "gltf.animation_path_unsupported"));
    REQUIRE(result.scene.unsupported_features.size() == 1);
    REQUIRE(result.scene.unsupported_features[0].feature ==
            "AnimationPath:weights");
    REQUIRE(result.scene.unsupported_features[0].node_path == "MoveSpinScale");
    REQUIRE(result.scene.animations.size() == 1);

    const auto& animation = result.scene.animations[0];
    REQUIRE(animation.name == "MoveSpinScale");
    REQUIRE(animation.samplers.size() == 3);
    REQUIRE(animation.channels.size() == 3);

    REQUIRE(animation.channels[0].node == 0);
    REQUIRE(animation.channels[0].sampler == 0);
    REQUIRE(animation.channels[0].path == AnimationChannelData::Path::translation);
    REQUIRE(animation.samplers[0].interpolation ==
            AnimationSamplerData::Interpolation::linear);
    REQUIRE(animation.samplers[0].input_times == std::vector<float>{0.0f, 1.0f});
    REQUIRE(animation.samplers[0].output_components == 3);
    REQUIRE(animation.samplers[0].output_values ==
            std::vector<float>{0.0f, 0.0f, 0.0f, 2.0f, 3.0f, 4.0f});

    REQUIRE(animation.channels[1].node == 0);
    REQUIRE(animation.channels[1].sampler == 1);
    REQUIRE(animation.channels[1].path == AnimationChannelData::Path::rotation);
    REQUIRE(animation.samplers[1].interpolation ==
            AnimationSamplerData::Interpolation::step);
    REQUIRE(animation.samplers[1].output_components == 4);
    REQUIRE(animation.samplers[1].output_values ==
            std::vector<float>{0.0f, 0.0f, 0.0f, 1.0f,
                               0.0f, 0.70710677f, 0.0f, 0.70710677f});

    REQUIRE(animation.channels[2].node == 0);
    REQUIRE(animation.channels[2].sampler == 2);
    REQUIRE(animation.channels[2].path == AnimationChannelData::Path::scale);
    REQUIRE(animation.samplers[2].output_components == 3);
    REQUIRE(animation.samplers[2].output_values ==
            std::vector<float>{1.0f, 1.0f, 1.0f, 2.0f, 2.0f, 2.0f});
}

TEST_CASE("load_gltf_scene reports weights-only animations as native gaps",
          "[scene3d][gltf]") {
    const auto dir = std::filesystem::temp_directory_path() /
        "pulp-scene3d-weights-animation-gltf";
    std::filesystem::create_directories(dir);

    std::vector<uint8_t> bin;
    append_f32(bin, 0.0f);
    append_f32(bin, 1.0f);
    append_f32(bin, 0.0f);
    append_f32(bin, 1.0f);
    write_file(dir / "weights.bin", bin);

    const auto gltf_path = dir / "weights.gltf";
    write_file(
        gltf_path,
        R"({"asset":{"version":"2.0","generator":"pulp-test-scene3d-weights-animation"},"buffers":[{"uri":"weights.bin","byteLength":16}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":8},{"buffer":0,"byteOffset":8,"byteLength":8}],"accessors":[{"bufferView":0,"componentType":5126,"count":2,"type":"SCALAR","min":[0],"max":[1]},{"bufferView":1,"componentType":5126,"count":2,"type":"SCALAR"}],"nodes":[{"name":"morph-target"}],"animations":[{"name":"MorphWeightsOnly","samplers":[{"input":0,"output":1,"interpolation":"LINEAR"}],"channels":[{"sampler":0,"target":{"node":0,"path":"weights"}}]}],"scenes":[{"nodes":[0]}],"scene":0})");

    auto result = load_gltf_scene(gltf_path);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(has_code_path(result.scene.diagnostics,
                          "gltf.animation_path_unsupported",
                          gltf_path));
    REQUIRE(result.scene.animations.empty());
    REQUIRE(result.scene.unsupported_features.size() == 1);
    REQUIRE(result.scene.unsupported_features[0].feature ==
            "AnimationPath:weights");
    REQUIRE(result.scene.unsupported_features[0].node_path ==
            "MorphWeightsOnly");

    const auto sidecar = build_sidecar_from_scene(result.scene);
    REQUIRE(sidecar.unsupported_features.size() == 1);
    REQUIRE(sidecar.unsupported_features[0].feature == "AnimationPath:weights");

    const auto report = analyze_bake_preflight(sidecar);
    REQUIRE_FALSE(report.export_blocked);
    REQUIRE(report.native_runtime_has_gaps);
    REQUIRE(report.native_runtime_gaps.size() == 1);
    REQUIRE(report.native_runtime_gaps[0].feature == "AnimationPath:weights");
    REQUIRE(std::string(bake_preflight_readiness(report)) == "native_gaps");
}

TEST_CASE("load_gltf_scene reports malformed GLB parse diagnostics",
          "[scene3d][gltf]") {
    auto path = write_temp_file("pulp-scene3d-malformed.glb",
                                std::vector<uint8_t>{'n', 'o', 't', 'g', 'l', 'b'});

    auto result = load_gltf_scene(path);

    REQUIRE_FALSE(result.success);
    REQUIRE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(has_code(result.scene.diagnostics, "gltf.parse_failed"));
    REQUIRE_FALSE(result.error.empty());
}

TEST_CASE("load_gltf_scene routes Draco compression through loader callback",
          "[scene3d][gltf][draco]") {
    const auto dir = std::filesystem::temp_directory_path() /
        "pulp-scene3d-draco-diagnostic";
    std::filesystem::create_directories(dir);
    const auto bin_path = dir / "mesh.bin";
    std::vector<uint8_t> bin;
    append_f32(bin, -1.0f); append_f32(bin, -1.0f); append_f32(bin, 0.0f);
    append_f32(bin, 1.0f); append_f32(bin, -1.0f); append_f32(bin, 0.0f);
    append_f32(bin, 0.0f); append_f32(bin, 1.0f); append_f32(bin, 0.0f);
    append_u16(bin, 0); append_u16(bin, 1); append_u16(bin, 2);
    bin.push_back(0);
    bin.push_back(1);
    bin.push_back(2);
    bin.push_back(3);
    write_file(bin_path, bin);
    const auto gltf_path = dir / "draco.gltf";
    write_file(
        gltf_path,
        R"({"asset":{"version":"2.0","generator":"pulp-test-scene3d-draco"},"extensionsUsed":["KHR_draco_mesh_compression"],"extensionsRequired":["KHR_draco_mesh_compression"],"buffers":[{"uri":"mesh.bin","byteLength":46}],"bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":36,"target":34962},{"buffer":0,"byteOffset":36,"byteLength":6,"target":34963},{"buffer":0,"byteOffset":42,"byteLength":4}],"accessors":[{"bufferView":0,"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},{"bufferView":1,"componentType":5123,"count":3,"type":"SCALAR"}],"meshes":[{"name":"compressed","primitives":[{"attributes":{"POSITION":0},"indices":1,"mode":4,"extensions":{"KHR_draco_mesh_compression":{"bufferView":2,"attributes":{"POSITION":7,"NORMAL":8,"TEXCOORD_0":9,"TEXCOORD_1":10,"TANGENT":11,"COLOR_0":12}}}}]}],"nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})");

    auto result = load_gltf_scene(gltf_path);

    REQUIRE_FALSE(result.success);
    REQUIRE(has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(has_code(result.scene.diagnostics, "gltf.draco_not_wired"));

    LoadOptions options;
    options.allow_draco = false;
    auto disabled = load_gltf_scene(gltf_path, options);
    REQUIRE_FALSE(disabled.success);
    REQUIRE(has_code(disabled.scene.diagnostics, "gltf.draco_disabled"));

    bool callback_called = false;
    LoadOptions decode_options;
    decode_options.draco_decode =
        [&](const uint8_t* data,
            size_t size,
            const DracoAttributeIds& ids) {
            callback_called = true;
            CHECK(data != nullptr);
            CHECK(size == 4);
            CHECK(data[0] == 0);
            CHECK(data[1] == 1);
            CHECK(data[2] == 2);
            CHECK(data[3] == 3);
            CHECK(ids.position == 7);
            CHECK(ids.normal == 8);
            CHECK(ids.texcoord0 == 9);
            CHECK(ids.texcoord1 == 10);
            CHECK(ids.tangent == 11);
            CHECK(ids.color0 == 12);

            DracoDecodedMesh decoded;
            decoded.positions = {
                -1.0f, -1.0f, 0.0f,
                 1.0f, -1.0f, 0.0f,
                 0.0f,  1.0f, 0.0f,
            };
            decoded.normals = {
                0.0f, 0.0f, 1.0f,
                0.0f, 0.0f, 1.0f,
                0.0f, 0.0f, 1.0f,
            };
            decoded.texcoord0 = {
                0.0f, 0.0f,
                1.0f, 0.0f,
                0.5f, 1.0f,
            };
            decoded.texcoord1 = {
                0.25f, 0.25f,
                0.75f, 0.25f,
                0.5f, 0.75f,
            };
            decoded.tangents = {
                1.0f, 0.0f, 0.0f, 1.0f,
                1.0f, 0.0f, 0.0f, 1.0f,
                1.0f, 0.0f, 0.0f, 1.0f,
            };
            decoded.color0 = {
                1.0f, 0.0f, 0.0f, 1.0f,
                0.0f, 1.0f, 0.0f, 1.0f,
                0.0f, 0.0f, 1.0f, 1.0f,
            };
            decoded.indices = {0, 1, 2};
            decoded.success = true;
            return decoded;
        };

    auto decoded = load_gltf_scene(gltf_path, decode_options);

    REQUIRE(callback_called);
    REQUIRE(decoded.success);
    REQUIRE_FALSE(has_error_diagnostics(decoded.scene.diagnostics));
    REQUIRE(decoded.scene.meshes.size() == 1);
    REQUIRE(decoded.scene.meshes[0].primitives.size() == 1);
    const auto& primitive = decoded.scene.meshes[0].primitives[0];
    REQUIRE(primitive.positions.size() == 9);
    REQUIRE(primitive.normals.size() == 9);
    REQUIRE(primitive.texcoord0.size() == 6);
    REQUIRE(primitive.texcoord1.size() == 6);
    REQUIRE(primitive.tangents.size() == 12);
    REQUIRE(primitive.color0.size() == 12);
    REQUIRE(primitive.indices == std::vector<uint32_t>{0, 1, 2});

    LoadOptions unavailable_options;
    unavailable_options.draco_decode =
        [](const uint8_t*,
           size_t,
           const DracoAttributeIds&) {
            DracoDecodedMesh decoded;
            decoded.decoder_available = false;
            return decoded;
        };
    auto unavailable = load_gltf_scene(gltf_path, unavailable_options);
    REQUIRE_FALSE(unavailable.success);
    REQUIRE(has_code(unavailable.scene.diagnostics, "gltf.draco_unavailable"));
    REQUIRE_FALSE(has_code(unavailable.scene.diagnostics, "gltf.draco_decode_failed"));
}
