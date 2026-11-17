// metal_renderer_mac.mm: Vellum's Metal RmlUi renderer on an RmlUiSurface.
// Compiled with ARC.

#include <pulp/rmlui_native/metal_renderer.hpp>

#include <vellum/rmlui/native.hpp>

namespace pulp::rmlui_native {

vellum::rmlui::MetalFrameTarget metal_frame_target(const FrameTarget& frame) {
    vellum::rmlui::MetalFrameTarget target;
    target.command_buffer = (__bridge id<MTLCommandBuffer>)frame.command_buffer;
    target.target = (__bridge id<MTLTexture>)frame.texture;
    target.viewport = MTLViewport{0.0, 0.0, double(frame.width), double(frame.height), 0.0, 1.0};
    // The context is sized in drawable pixels. Density remains available to
    // RmlUi for dp layout, while the renderer receives pixel coordinates.
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
#if defined(VELLUM_RMLUI_NATIVE_API_VERSION) &&                                                    \
    (VELLUM_RMLUI_NATIVE_FEATURES & VELLUM_RMLUI_NATIVE_FEATURE_RENDERER_TRIM)
    // The surface hides an editor with no frame being encoded and after the last
    // committed frame has completed, which is what trim() asks for. An older
    // Vellum without the feature bit leaves its render targets until it is
    // destroyed.
    hooks.trim = [&renderer] { renderer.trim(); };
#endif
    return hooks;
}

} // namespace pulp::rmlui_native
