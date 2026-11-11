#pragma once

/// @file metal_renderer.hpp
/// Vellum's Metal `Rml::RenderInterface` (`VellumRmlUi::Metal`) on an
/// `RmlUiSurface`: the hooks that hand it each frame's command buffer and
/// drawable texture.
///
/// Build the renderer on the device the surface uses, one renderer per
/// surface (its layer, stencil and filter textures are sized to that surface's
/// drawable, and its pixel format is fixed at the first frame):
///
///     id<MTLDevice> device = MTLCreateSystemDefaultDevice();
///     vellum::rmlui::MetalRenderInterface renderer(device);
///     RmlUiSurfaceOptions options;
///     options.device = (__bridge void*)device;
///     RmlUiSurface ui(assets, renderer, metal_renderer_hooks(renderer), options);
///
/// The surface presents and commits; the renderer only encodes. The drawable
/// is BGRA8Unorm, never an sRGB format: RmlUi produces sRGB-encoded colours
/// and blends them as they are, which a linear-space attachment would change.
///
/// Available when Pulp is configured with `PULP_ENABLE_RMLUI_ADAPTER=ON`
/// against a Vellum install built with `VELLUM_RMLUI_METAL=ON`
/// (target `pulp::rmlui-native-metal`).

#include <pulp/rmlui_native/rmlui_surface.hpp>

namespace vellum::rmlui {
class MetalRenderInterface;
}

namespace pulp::rmlui_native {

/// Hooks that begin and end a `renderer` frame around RmlUi's render. The
/// renderer must outlive the surface the hooks are given to.
RendererHooks metal_renderer_hooks(vellum::rmlui::MetalRenderInterface& renderer);

} // namespace pulp::rmlui_native

#if defined(__OBJC__)
#include <vellum/rmlui/metal_render_interface.hpp>

namespace pulp::rmlui_native {

/// The renderer's view of one surface frame: the whole drawable as the
/// viewport, one target pixel per RmlUi context unit (the surface sizes the
/// context in drawable pixels and scales dp through RmlUi's density ratio),
/// and a cleared target (a drawable's previous contents are undefined).
vellum::rmlui::MetalFrameTarget metal_frame_target(const FrameTarget& frame);

} // namespace pulp::rmlui_native
#endif
