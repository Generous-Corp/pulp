#pragma once

#include <cstdint>
#include <vector>

namespace pulp::render {

/// Decoded mesh geometry from DRACO-compressed data.
struct DecodedMesh {
    std::vector<float> positions;      ///< Flat xyz positions
    std::vector<float> normals;        ///< Flat xyz normals (may be empty)
    std::vector<float> tex_coords;     ///< Flat uv coordinates (may be empty)
    std::vector<uint32_t> indices;     ///< Triangle indices
    int vertex_count = 0;
    int face_count = 0;
    bool success = false;
};

/// Decode a DRACO-compressed mesh buffer.
/// Returns decoded geometry with positions, normals, UVs, and indices.
DecodedMesh decode_draco(const uint8_t* data, size_t size);

} // namespace pulp::render
