#include <pulp/vellum_scene/draco_scene_adapter.hpp>

#include <utility>

namespace pulp::vellum_scene {

namespace {

::vellum::render::DecodedMesh decode_with_ids(const std::uint8_t* data, std::size_t size,
                                              const ::vellum::render::DracoAttributeIds& ids) {
    return ::vellum::render::decode_draco(data, size, ids);
}

} // namespace

scene::DracoDecodeCallback make_draco_decode_callback(const DracoDecoderHooks& hooks) {
    return [hooks](const std::uint8_t* data, std::size_t size,
                   const scene::DracoAttributeIds& scene_ids) {
        ::vellum::render::DracoAttributeIds ids;
        ids.position = scene_ids.position;
        ids.normal = scene_ids.normal;
        ids.texcoord0 = scene_ids.texcoord0;
        ids.texcoord1 = scene_ids.texcoord1;
        ids.tangent = scene_ids.tangent;
        ids.color0 = scene_ids.color0;

        auto decoded = hooks.decode(data, size, ids);

        scene::DracoDecodedMesh out;
        out.positions = std::move(decoded.positions);
        out.normals = std::move(decoded.normals);
        out.texcoord0 = std::move(decoded.tex_coords);
        out.texcoord1 = std::move(decoded.tex_coords1);
        out.tangents = std::move(decoded.tangents);
        out.color0 = std::move(decoded.colors);
        out.indices = std::move(decoded.indices);
        out.decoder_available = hooks.available();
        // A decode that did not apply the glTF unique ids has the wrong
        // attributes, so it does not count as a decode.
        out.success = decoded.success && decoded.unique_id_attributes_applied;
        return out;
    };
}

scene::DracoDecodeCallback make_vellum_draco_decode_callback() {
    return make_draco_decode_callback(
        DracoDecoderHooks{&decode_with_ids, &::vellum::render::draco_decoder_available});
}

bool vellum_draco_decoder_available() {
    return ::vellum::render::draco_decoder_available();
}

} // namespace pulp::vellum_scene
