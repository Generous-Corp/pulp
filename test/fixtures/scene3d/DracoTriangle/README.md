# DracoTriangle fixture

`DracoTriangle.glb` is one triangle whose geometry exists only as a
`KHR_draco_mesh_compression` buffer view: POSITION, NORMAL and TEXCOORD_0 under
Draco unique ids 7, 8 and 9, no uncompressed fallback. It is a Pulp-authored
test asset with no third-party content; the Draco encoder (Apache-2.0, tag
1.5.9) was used only to produce it and is not part of the repository.

Decoded content:

- positions `(-1,-1,0) (1,-1,0) (0,1,0)`
- normals `(0,0,1)` for all three vertices
- texcoord0 `(0,0) (1,0) (0.5,1)`
- indices `0 1 2`

SHA-256: `2fcef8c1282cfe1bd682cc5ed1e4ef7a565cdc7dc174f3e76edd4b9ab6e64411`

Regenerate it by building the program below against Draco 1.5.9
(`add_subdirectory(draco)` with `DRACO_TRANSCODER_SUPPORTED`, `DRACO_TESTS`,
`DRACO_JS_GLUE` and `DRACO_WASM` off) and running it with the output path.
The position attribute is stored unquantized so the decoded values are exact.

```cpp
// Writes DracoTriangle.glb: one triangle, POSITION/NORMAL/TEXCOORD_0 stored
// only as a KHR_draco_mesh_compression buffer view (Draco unique ids 7, 8, 9).
#include <draco/attributes/geometry_attribute.h>
#include <draco/compression/encode.h>
#include <draco/core/encoder_buffer.h>
#include <draco/core/vector_d.h>
#include <draco/mesh/triangle_soup_mesh_builder.h>

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

static void put_u32(std::vector<uint8_t>& v, uint32_t x) {
    for (int i = 0; i < 4; ++i) v.push_back(uint8_t((x >> (8 * i)) & 0xff));
}

int main(int argc, char** argv) {
    if (argc != 2) return 2;
    draco::TriangleSoupMeshBuilder builder;
    builder.Start(1);
    const int pos = builder.AddAttribute(draco::GeometryAttribute::POSITION, 3, draco::DT_FLOAT32);
    const int nrm = builder.AddAttribute(draco::GeometryAttribute::NORMAL, 3, draco::DT_FLOAT32);
    const int uv = builder.AddAttribute(draco::GeometryAttribute::TEX_COORD, 2, draco::DT_FLOAT32);
    const draco::Vector3f p0(-1.f, -1.f, 0.f), p1(1.f, -1.f, 0.f), p2(0.f, 1.f, 0.f);
    const draco::Vector3f n(0.f, 0.f, 1.f);
    const draco::Vector2f t0(0.f, 0.f), t1(1.f, 0.f), t2(0.5f, 1.f);
    builder.SetAttributeValuesForFace(pos, draco::FaceIndex(0), p0.data(), p1.data(), p2.data());
    builder.SetAttributeValuesForFace(nrm, draco::FaceIndex(0), n.data(), n.data(), n.data());
    builder.SetAttributeValuesForFace(uv, draco::FaceIndex(0), t0.data(), t1.data(), t2.data());
    builder.SetAttributeUniqueId(pos, 7);
    builder.SetAttributeUniqueId(nrm, 8);
    builder.SetAttributeUniqueId(uv, 9);
    auto mesh = builder.Finalize();
    if (!mesh) return 1;
    draco::Encoder encoder;
    encoder.SetSpeedOptions(10, 10);
    // Lossless for the fixture's exact values.
    encoder.SetAttributeQuantization(draco::GeometryAttribute::POSITION, 0);
    draco::EncoderBuffer buf;
    if (!encoder.EncodeMeshToBuffer(*mesh, &buf).ok()) return 1;

    std::vector<uint8_t> bin(buf.data(), buf.data() + buf.size());
    while (bin.size() % 4) bin.push_back(0);
    const std::string json =
        R"({"asset":{"version":"2.0","generator":"DracoTriangle fixture"},)"
        R"("extensionsUsed":["KHR_draco_mesh_compression"],"extensionsRequired":["KHR_draco_mesh_compression"],)"
        R"("buffers":[{"byteLength":)" + std::to_string(bin.size()) + R"(}],)"
        R"("bufferViews":[{"buffer":0,"byteOffset":0,"byteLength":)" + std::to_string(buf.size()) + R"(}],)"
        R"("accessors":[{"componentType":5126,"count":3,"type":"VEC3","min":[-1,-1,0],"max":[1,1,0]},)"
        R"({"componentType":5126,"count":3,"type":"VEC3"},)"
        R"({"componentType":5126,"count":3,"type":"VEC2"},)"
        R"({"componentType":5125,"count":3,"type":"SCALAR"}],)"
        R"("meshes":[{"name":"triangle","primitives":[{"attributes":{"POSITION":0,"NORMAL":1,"TEXCOORD_0":2},"indices":3,"mode":4,)"
        R"("extensions":{"KHR_draco_mesh_compression":{"bufferView":0,"attributes":{"POSITION":7,"NORMAL":8,"TEXCOORD_0":9}}}}]}],)"
        R"("nodes":[{"mesh":0}],"scenes":[{"nodes":[0]}],"scene":0})";
    std::vector<uint8_t> j(json.begin(), json.end());
    while (j.size() % 4) j.push_back(' ');
    std::vector<uint8_t> glb;
    put_u32(glb, 0x46546C67);
    put_u32(glb, 2);
    put_u32(glb, uint32_t(12 + 8 + j.size() + 8 + bin.size()));
    put_u32(glb, uint32_t(j.size())); put_u32(glb, 0x4E4F534A);
    glb.insert(glb.end(), j.begin(), j.end());
    put_u32(glb, uint32_t(bin.size())); put_u32(glb, 0x004E4942);
    glb.insert(glb.end(), bin.begin(), bin.end());
    FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 1;
    std::fwrite(glb.data(), 1, glb.size(), f);
    std::fclose(f);
    std::printf("draco bytes=%zu glb bytes=%zu\n", size_t(buf.size()), glb.size());
    return 0;
}
```
