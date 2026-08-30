#pragma once

// Binds the render module's Draco decoder to the scene loader's decode hook.
// The adapter needs the complete decoder and loader types, so it requires both
// core/scene/include and core/render/include on the include path. It lives with
// the scene headers because those are not part of the installed SDK header set,
// which carries no pulp/scene headers to satisfy the include below.

#include <pulp/render/draco_decoder.hpp>
#include <pulp/scene/gltf_loader.hpp>

#include <cstddef>
#include <cstdint>
#include <utility>

namespace pulp::scene {

inline DracoDecodeCallback make_scene_draco_decode_callback() {
    return [](const uint8_t* data,
              size_t size,
              const DracoAttributeIds& scene_ids) {
        render::DracoAttributeIds render_ids;
        render_ids.position = scene_ids.position;
        render_ids.normal = scene_ids.normal;
        render_ids.texcoord0 = scene_ids.texcoord0;
        render_ids.texcoord1 = scene_ids.texcoord1;
        render_ids.tangent = scene_ids.tangent;
        render_ids.color0 = scene_ids.color0;

        auto decoded = render::decode_draco(data, size, render_ids);

        DracoDecodedMesh out;
        out.positions = std::move(decoded.positions);
        out.normals = std::move(decoded.normals);
        out.texcoord0 = std::move(decoded.tex_coords);
        out.texcoord1 = std::move(decoded.tex_coords1);
        out.tangents = std::move(decoded.tangents);
        out.color0 = std::move(decoded.colors);
        out.indices = std::move(decoded.indices);
        out.decoder_available = render::draco_decoder_available();
        out.success = decoded.success && decoded.unique_id_attributes_applied;
        return out;
    };
}

} // namespace pulp::scene
