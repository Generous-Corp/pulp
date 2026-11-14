// metal_renderer_mac.mm: Vellum's Metal RmlUi renderer on an RmlUiSurface.
// Compiled with ARC.

#include <pulp/rmlui_native/metal_renderer.hpp>

namespace pulp::rmlui_native {

vellum::rmlui::MetalFrameTarget metal_frame_target(const FrameTarget& frame) {
    vellum::rmlui::MetalFrameTarget target;
    target.command_buffer = (__bridge id<MTLCommandBuffer>)frame.command_buffer;
    target.target = (__bridge id<MTLTexture>)frame.texture;
    target.viewport = MTLViewport{0.0, 0.0, double(frame.width), double(frame.height), 0.0, 1.0};
    // RmlUi lays out in drawable pixels, while Vellum uses the backing scale
    // for its pixel-aware effects and text coverage. Preserve the surface's
    // scale so a 2x drawable gets the same renderer contract as a 1x frame.
    target.scale = static_cast<float>(frame.scale > 0.0 ? frame.scale : 1.0);
    target.clear = true;
    return target;
}

RendererHooks metal_renderer_hooks(vellum::rmlui::MetalRenderInterface& renderer) {
#if defined(PULP_VELLUM_RMLUI_HAS_ATLAS_FILTER)
    renderer.set_atlas_filter(vellum::rmlui::MetalRenderInterface::AtlasFilter::nearest);
#endif
    RendererHooks hooks;
    hooks.begin_frame = [&renderer](const FrameTarget& frame) {
        renderer.begin_frame(metal_frame_target(frame));
    };
    hooks.end_frame = [&renderer] { renderer.end_frame(); };
    return hooks;
}

} // namespace pulp::rmlui_native
