#include <catch2/catch_test_macros.hpp>

#include <pulp/scene/gltf_loader.hpp>
#include <pulp/vellum_scene/draco_scene_adapter.hpp>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

using namespace pulp::vellum_scene;
namespace scene = pulp::scene;

namespace {

std::filesystem::path fixture(const char* relative) {
    return std::filesystem::path(PULP_VELLUM_SCENE_FIXTURE_DIR) / relative;
}

std::vector<std::uint8_t> read_all(const std::filesystem::path& path) {
    std::ifstream in(path, std::ios::binary);
    return {std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
}

bool has_code(const std::vector<scene::Diagnostic>& diagnostics, const std::string& code) {
    return std::any_of(diagnostics.begin(), diagnostics.end(),
                       [&](const scene::Diagnostic& d) { return d.code == code; });
}

std::string describe(const std::vector<scene::Diagnostic>& diagnostics) {
    std::string text;
    for (const auto& d : diagnostics)
        text += d.code + ": " + d.message + "\n";
    return text;
}

// The triangle DracoTriangle.glb encodes: POSITION, NORMAL and TEXCOORD_0 under
// Draco unique ids 7, 8 and 9.
const std::vector<float> kPositions = {-1, -1, 0, 1, -1, 0, 0, 1, 0};
const std::vector<float> kNormals = {0, 0, 1, 0, 0, 1, 0, 0, 1};
const std::vector<float> kTexcoords = {0, 0, 1, 0, 0.5f, 1};
const std::vector<std::uint32_t> kIndices = {0, 1, 2};

// Records what the loader hands the decoder and answers with the triangle.
struct FakeDecoder {
    static inline int calls = 0;
    static inline std::size_t last_size = 0;
    static inline std::vector<std::uint8_t> last_head;
    static inline ::vellum::render::DracoAttributeIds last_ids;
    static inline bool applied = true;
    static inline bool succeed = true;
    // Also return the secondary uv, tangent and colour streams, which the
    // fixture's three-attribute primitive does not carry.
    static inline bool full = false;

    static void reset() {
        full = false;
        calls = 0;
        last_size = 0;
        last_head.clear();
        last_ids = {};
        applied = true;
        succeed = true;
    }
    static ::vellum::render::DecodedMesh decode(const std::uint8_t* data, std::size_t size,
                                                const ::vellum::render::DracoAttributeIds& ids) {
        ++calls;
        last_size = size;
        last_head.assign(data, data + std::min<std::size_t>(size, 5));
        last_ids = ids;
        ::vellum::render::DecodedMesh mesh;
        mesh.positions = kPositions;
        mesh.normals = kNormals;
        mesh.tex_coords = kTexcoords;
        if (full) {
            mesh.tex_coords1 = {0.25f, 0.25f};
            mesh.tangents = {1, 0, 0, 1};
            mesh.colors = {1, 0, 0, 1};
        }
        mesh.indices = kIndices;
        mesh.vertex_count = 3;
        mesh.face_count = 1;
        mesh.success = succeed;
        mesh.unique_id_attributes_applied = applied;
        return mesh;
    }
    static bool available() {
        return true;
    }
    static DracoDecoderHooks hooks() {
        return {&decode, &available};
    }
};

} // namespace

TEST_CASE("the adapter maps ids out and decoded fields back", "[vellum-scene][draco]") {
    FakeDecoder::reset();
    FakeDecoder::full = true;
    auto callback = make_draco_decode_callback(FakeDecoder::hooks());

    scene::DracoAttributeIds ids;
    ids.position = 7;
    ids.normal = 8;
    ids.texcoord0 = 9;
    ids.texcoord1 = 10;
    ids.tangent = 11;
    ids.color0 = 12;
    const std::uint8_t bytes[] = {'D', 'R', 'A', 'C', 'O', 1, 2};
    const auto out = callback(bytes, sizeof(bytes), ids);

    CHECK(FakeDecoder::calls == 1);
    CHECK(FakeDecoder::last_size == sizeof(bytes));
    CHECK(FakeDecoder::last_ids.position == 7);
    CHECK(FakeDecoder::last_ids.normal == 8);
    CHECK(FakeDecoder::last_ids.texcoord0 == 9);
    CHECK(FakeDecoder::last_ids.texcoord1 == 10);
    CHECK(FakeDecoder::last_ids.tangent == 11);
    CHECK(FakeDecoder::last_ids.color0 == 12);

    CHECK(out.success);
    CHECK(out.decoder_available);
    CHECK(out.positions == kPositions);
    CHECK(out.normals == kNormals);
    CHECK(out.texcoord0 == kTexcoords);
    CHECK(out.texcoord1 == std::vector<float>{0.25f, 0.25f});
    CHECK(out.tangents == std::vector<float>{1, 0, 0, 1});
    CHECK(out.color0 == std::vector<float>{1, 0, 0, 1});
    CHECK(out.indices == kIndices);
}

TEST_CASE("a decode that did not apply the glTF unique ids is not a success",
          "[vellum-scene][draco]") {
    FakeDecoder::reset();
    FakeDecoder::applied = false;
    auto callback = make_draco_decode_callback(FakeDecoder::hooks());
    const std::uint8_t bytes[] = {1, 2, 3};
    CHECK_FALSE(callback(bytes, sizeof(bytes), {}).success);

    FakeDecoder::reset();
    FakeDecoder::succeed = false;
    auto failing = make_draco_decode_callback(FakeDecoder::hooks());
    CHECK_FALSE(failing(bytes, sizeof(bytes), {}).success);
}

TEST_CASE("the Vellum callback refuses data that is not a Draco mesh", "[vellum-scene][draco]") {
    auto callback = make_vellum_draco_decode_callback();
    const std::uint8_t garbage[] = {0, 1, 2, 3};
    const auto out = callback(garbage, sizeof(garbage), {});
    CHECK_FALSE(out.success);
    CHECK(out.decoder_available == vellum_draco_decoder_available());
    CHECK(out.positions.empty());
    CHECK(out.indices.empty());

    const auto empty = callback(nullptr, 0, {});
    CHECK_FALSE(empty.success);
}

TEST_CASE("the loader reads the BoxTextured glTF fixture", "[vellum-scene][gltf]") {
    const auto path = fixture("BoxTextured/BoxTextured.glb");
    REQUIRE(std::filesystem::exists(path));

    scene::LoadOptions options;
    options.draco_decode = make_vellum_draco_decode_callback();
    const auto result = scene::load_gltf_scene(path, options);

    INFO(result.error);
    REQUIRE(result.success);
    REQUIRE_FALSE(scene::has_error_diagnostics(result.scene.diagnostics));
    REQUIRE(result.scene.meshes.size() == 1);
    REQUIRE(result.scene.meshes[0].primitives.size() == 1);
    const auto& primitive = result.scene.meshes[0].primitives[0];
    CHECK(primitive.positions.size() == 24u * 3u);
    CHECK(primitive.normals.size() == 24u * 3u);
    CHECK(primitive.texcoord0.size() == 24u * 2u);
    CHECK(primitive.indices.size() == 36u);
    REQUIRE(result.scene.textures.size() == 1);
    CHECK(result.scene.textures[0].mime_type == "image/png");
    CHECK_FALSE(result.scene.textures[0].encoded_bytes.empty());
}

TEST_CASE("a Draco glTF reaches the decoder with the glTF ids and the buffer view bytes",
          "[vellum-scene][gltf][draco]") {
    const auto path = fixture("DracoTriangle/DracoTriangle.glb");
    REQUIRE(std::filesystem::exists(path));

    FakeDecoder::reset();
    scene::LoadOptions options;
    options.draco_decode = make_draco_decode_callback(FakeDecoder::hooks());
    const auto result = scene::load_gltf_scene(path, options);

    INFO(result.error << "\n" << describe(result.scene.diagnostics));
    REQUIRE(result.success);
    REQUIRE_FALSE(scene::has_error_diagnostics(result.scene.diagnostics));
    CHECK(FakeDecoder::calls == 1);
    // The bytes are the Draco stream, not the file: it starts with the magic.
    REQUIRE(FakeDecoder::last_head.size() == 5);
    CHECK(std::string(FakeDecoder::last_head.begin(), FakeDecoder::last_head.end()) == "DRACO");
    CHECK(FakeDecoder::last_size > 0);
    CHECK(FakeDecoder::last_size < read_all(path).size());
    CHECK(FakeDecoder::last_ids.position == 7);
    CHECK(FakeDecoder::last_ids.normal == 8);
    CHECK(FakeDecoder::last_ids.texcoord0 == 9);
    CHECK(FakeDecoder::last_ids.texcoord1 == -1);

    REQUIRE(result.scene.meshes.size() == 1);
    REQUIRE(result.scene.meshes[0].primitives.size() == 1);
    const auto& primitive = result.scene.meshes[0].primitives[0];
    CHECK(primitive.positions == kPositions);
    CHECK(primitive.normals == kNormals);
    CHECK(primitive.texcoord0 == kTexcoords);
    CHECK(primitive.indices == kIndices);
}

TEST_CASE("a Draco glTF fails to load when the decoder fails", "[vellum-scene][gltf][draco]") {
    const auto path = fixture("DracoTriangle/DracoTriangle.glb");
    FakeDecoder::reset();
    FakeDecoder::succeed = false;
    scene::LoadOptions options;
    options.draco_decode = make_draco_decode_callback(FakeDecoder::hooks());
    const auto result = scene::load_gltf_scene(path, options);
    CHECK_FALSE(result.success);
    CHECK(has_code(result.scene.diagnostics, "gltf.draco_decode_failed"));
}

TEST_CASE("a Draco glTF does not load without a decoder or with Draco disabled",
          "[vellum-scene][gltf][draco]") {
    const auto path = fixture("DracoTriangle/DracoTriangle.glb");

    const auto unwired = scene::load_gltf_scene(path);
    CHECK_FALSE(unwired.success);
    CHECK(has_code(unwired.scene.diagnostics, "gltf.draco_not_wired"));

    FakeDecoder::reset();
    scene::LoadOptions disabled;
    disabled.allow_draco = false;
    disabled.draco_decode = make_draco_decode_callback(FakeDecoder::hooks());
    const auto refused = scene::load_gltf_scene(path, disabled);
    CHECK_FALSE(refused.success);
    CHECK(has_code(refused.scene.diagnostics, "gltf.draco_disabled"));
    CHECK(FakeDecoder::calls == 0);
}

TEST_CASE("the installed Vellum decoder loads the Draco fixture or says it cannot",
          "[vellum-scene][gltf][draco]") {
    const auto path = fixture("DracoTriangle/DracoTriangle.glb");
    scene::LoadOptions options;
    options.draco_decode = make_vellum_draco_decode_callback();
    const auto result = scene::load_gltf_scene(path, options);

    if (vellum_draco_decoder_available()) {
        INFO(result.error);
        REQUIRE(result.success);
        REQUIRE(result.scene.meshes.size() == 1);
        const auto& primitive = result.scene.meshes[0].primitives.at(0);
        CHECK(primitive.positions == kPositions);
        CHECK(primitive.normals == kNormals);
        CHECK(primitive.texcoord0 == kTexcoords);
        CHECK(primitive.indices == kIndices);
    } else {
        // An SDK built without Draco must refuse the asset by name, never
        // return an empty mesh as a success.
        CHECK_FALSE(result.success);
        CHECK(has_code(result.scene.diagnostics, "gltf.draco_unavailable"));
        UNSCOPED_INFO("the installed Vellum SDK ships a decoder built without Draco");
    }
}

TEST_CASE("a corrupted Draco payload never loads", "[vellum-scene][gltf][draco]") {
    // Overwrite the Draco stream inside a copy of the fixture with zeros. With
    // a decoder built without Draco this reduces to the unavailable case, so
    // the decode_failed path is only reached on a Draco-enabled SDK; the
    // fake-decoder failure case above covers the loader's side of it.
    auto bytes = read_all(fixture("DracoTriangle/DracoTriangle.glb"));
    const std::string magic = "DRACO";
    const auto at = std::search(bytes.begin(), bytes.end(), magic.begin(), magic.end());
    REQUIRE(at != bytes.end());
    std::fill(at, at + 32, std::uint8_t{0});

    const auto dir = std::filesystem::temp_directory_path() / "pulp-vellum-scene-corrupt";
    std::filesystem::create_directories(dir);
    const auto corrupt = dir / "corrupt.glb";
    {
        std::ofstream out(corrupt, std::ios::binary);
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    scene::LoadOptions options;
    options.draco_decode = make_vellum_draco_decode_callback();
    const auto result = scene::load_gltf_scene(corrupt, options);
    CHECK_FALSE(result.success);
    CHECK((has_code(result.scene.diagnostics, "gltf.draco_decode_failed") ||
           has_code(result.scene.diagnostics, "gltf.draco_unavailable")));
    std::filesystem::remove(corrupt);
}
