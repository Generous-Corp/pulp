#pragma once

// Skia calls that do counted resource work (see resource_counters.hpp). Using
// these instead of the bare Skia entry points keeps the count next to the
// work, so a decode or upload site cannot be added without being counted.

#include <pulp/canvas/resource_counters.hpp>
#include <pulp/canvas/skia_canvas.hpp>

#include "include/core/SkData.h"
#include "include/core/SkImage.h"
#if PULP_CANVAS_GRAPHITE
#include "include/gpu/graphite/Image.h"
#endif
#if PULP_CANVAS_GANESH
#include "include/gpu/ganesh/SkImageGanesh.h"
#endif

#include <utility>

namespace pulp::canvas {

inline sk_sp<SkImage> decode_encoded_image(sk_sp<SkData> data) {
    note_image_decode();
    return SkImages::DeferredFromEncodedData(std::move(data));
}

// Uploads through a Graphite recorder or a Ganesh direct context; counts only
// an upload that produced a texture.
#if PULP_CANVAS_GRAPHITE
inline sk_sp<SkImage> upload_texture_image(skgpu::graphite::Recorder* recorder,
                                           const SkImage* image) {
    auto texture = SkImages::TextureFromImage(recorder, image);
    if (texture)
        note_gpu_upload();
    return texture;
}
#endif
#if PULP_CANVAS_GANESH
inline sk_sp<SkImage> upload_texture_image(GrDirectContext* context, const SkImage* image) {
    auto texture = SkImages::TextureFromImage(context, image);
    if (texture)
        note_gpu_upload();
    return texture;
}
#endif

} // namespace pulp::canvas
