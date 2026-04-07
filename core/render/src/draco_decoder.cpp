#include <pulp/render/draco_decoder.hpp>

#ifdef PULP_HAS_DRACO

#include <draco/compression/decode.h>
#include <draco/mesh/mesh.h>
#include <draco/core/decoder_buffer.h>

namespace pulp::render {

DecodedMesh decode_draco(const uint8_t* data, size_t size) {
    DecodedMesh result;
    if (!data || size == 0) return result;

    draco::DecoderBuffer buffer;
    buffer.Init(reinterpret_cast<const char*>(data), size);

    auto type = draco::Decoder::GetEncodedGeometryType(&buffer);
    if (!type.ok() || type.value() != draco::TRIANGULAR_MESH) {
        return result;
    }

    draco::Decoder decoder;
    auto mesh_or = decoder.DecodeMeshFromBuffer(&buffer);
    if (!mesh_or.ok()) return result;

    auto& mesh = mesh_or.value();
    result.vertex_count = static_cast<int>(mesh->num_points());
    result.face_count = static_cast<int>(mesh->num_faces());

    // Extract positions
    auto* pos_attr = mesh->GetNamedAttribute(draco::GeometryAttribute::POSITION);
    if (pos_attr) {
        result.positions.resize(static_cast<size_t>(result.vertex_count) * 3);
        for (draco::PointIndex i(0); i < mesh->num_points(); ++i) {
            float values[3];
            pos_attr->GetMappedValue(i, values);
            auto idx = static_cast<size_t>(i.value()) * 3;
            result.positions[idx] = values[0];
            result.positions[idx + 1] = values[1];
            result.positions[idx + 2] = values[2];
        }
    }

    // Extract normals
    auto* norm_attr = mesh->GetNamedAttribute(draco::GeometryAttribute::NORMAL);
    if (norm_attr) {
        result.normals.resize(static_cast<size_t>(result.vertex_count) * 3);
        for (draco::PointIndex i(0); i < mesh->num_points(); ++i) {
            float values[3];
            norm_attr->GetMappedValue(i, values);
            auto idx = static_cast<size_t>(i.value()) * 3;
            result.normals[idx] = values[0];
            result.normals[idx + 1] = values[1];
            result.normals[idx + 2] = values[2];
        }
    }

    // Extract texture coordinates
    auto* tc_attr = mesh->GetNamedAttribute(draco::GeometryAttribute::TEX_COORD);
    if (tc_attr) {
        result.tex_coords.resize(static_cast<size_t>(result.vertex_count) * 2);
        for (draco::PointIndex i(0); i < mesh->num_points(); ++i) {
            float values[2];
            tc_attr->GetMappedValue(i, values);
            auto idx = static_cast<size_t>(i.value()) * 2;
            result.tex_coords[idx] = values[0];
            result.tex_coords[idx + 1] = values[1];
        }
    }

    // Extract indices
    result.indices.resize(static_cast<size_t>(result.face_count) * 3);
    for (draco::FaceIndex i(0); i < mesh->num_faces(); ++i) {
        auto& face = mesh->face(i);
        auto idx = static_cast<size_t>(i.value()) * 3;
        result.indices[idx] = face[0].value();
        result.indices[idx + 1] = face[1].value();
        result.indices[idx + 2] = face[2].value();
    }

    result.success = true;
    return result;
}

} // namespace pulp::render

#else // !PULP_HAS_DRACO

namespace pulp::render {

DecodedMesh decode_draco(const uint8_t*, size_t) {
    return {};  // DRACO not available
}

} // namespace pulp::render

#endif
