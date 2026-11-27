#pragma once

// Binds Vellum's Draco decoder to the glTF loader's decode hook.
//
// pulp::scene loads KHR_draco_mesh_compression primitives through a callback
// (scene::LoadOptions::draco_decode). make_vellum_draco_decode_callback()
// returns one that decodes with vellum::render::decode_draco, so a scene load
// needs only the installed Vellum SDK. The decoder an SDK ships may be built
// without Draco; the callback then reports decoder_available == false and the
// loader answers with a gltf.draco_unavailable diagnostic instead of a mesh.

#include <pulp/scene/gltf_loader.hpp>
#include <vellum/render/draco_decoder.hpp>

#include <cstddef>
#include <cstdint>

namespace pulp::vellum_scene {

/// A callback that decodes with Vellum's decoder.
scene::DracoDecodeCallback make_vellum_draco_decode_callback();

/// The decoder entry points the callback calls.
struct DracoDecoderHooks {
    ::vellum::render::DecodedMesh (*decode)(const std::uint8_t* data, std::size_t size,
                                            const ::vellum::render::DracoAttributeIds& ids) =
        nullptr;
    bool (*available)() = nullptr;
};

/// The same mapping over caller-supplied entry points, so the field and id
/// mapping can be exercised without a Draco-enabled SDK.
scene::DracoDecodeCallback make_draco_decode_callback(const DracoDecoderHooks& hooks);

/// True when the installed Vellum decoder was built with Draco.
bool vellum_draco_decoder_available();

} // namespace pulp::vellum_scene
