// metal_renderer_mac.mm: Vellum's Metal RmlUi renderer on an RmlUiSurface.
// Compiled with ARC.

#include <pulp/rmlui_native/metal_renderer.hpp>

namespace pulp::rmlui_native {

vellum::rmlui::MetalFrameTarget metal_frame_target(const FrameTarget& frame) {
    vellum::rmlui::MetalFrameTarget target;
    target.command_buffer = (__bridge id<MTLCommandBuffer>)frame.command_buffer;
    target.target = (__bridge id<MTLTexture>)frame.texture;
    target.viewport = MTLViewport{0.0, 0.0, double(frame.width), double(frame.height), 0.0, 1.0};
    // The surface sizes the RmlUi context in drawable pixels and maps dp to
    // points through RmlUi's density ratio, so a context unit already is a
    // target pixel. The backing scale must not be applied a second time.
    target.scale = 1.0f;
    target.clear = true;
    return target;
}

RendererHooks metal_renderer_hooks(vellum::rmlui::MetalRenderInterface& renderer) {
    RendererHooks hooks;
    hooks.begin_frame = [&renderer](const FrameTarget& frame) {
        renderer.begin_frame(metal_frame_target(frame));
    };
    hooks.end_frame = [&renderer] { renderer.end_frame(); };
    return hooks;
}

} // namespace pulp::rmlui_native
