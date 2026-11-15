#pragma once

/// @file rmlui_surface.hpp
/// RmlUi on a pulp-native Metal surface: the glue between Vellum's open RmlUi
/// entry point (`VellumRmlUi::Native`) and `pulp::native`.
///
/// An `RmlUiSurface` owns a `MetalSurface`, a `FramePump` and a
/// `vellum::rmlui::NativeContext`. It translates the surface's input into
/// RmlUi input, drives `update()` and `render()` from the pump, keeps the
/// context's size and pixel ratio in step with the surface, and tears all of
/// it down in order. It contains no renderer: the caller supplies any
/// `Rml::RenderInterface` and two hooks around each frame that hand that
/// renderer the frame's Metal command buffer and target texture.
///
/// Threading: RmlUi's state is process-wide and not thread-safe, so every call
/// into RmlUi in this binary, from any surface, input or render, holds
/// `rmlui_mutex()`. A caller that calls RmlUi itself (data models, element
/// queries) uses `with_context()`, which takes it. Members other than
/// `with_context()` run on the main thread.

#include <pulp/native/frame_pump.hpp>
#include <pulp/native/metal_surface.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace Rml {
class Context;
class RenderInterface;
} // namespace Rml

namespace vellum::rmlui {
class AssetProvider;
}

namespace pulp::rmlui_native {

/// The lock every RmlUi call in this binary takes.
std::mutex& rmlui_mutex();

/// One frame's target, handed to the renderer before RmlUi renders into it.
struct FrameTarget {
    void* command_buffer = nullptr; ///< `id<MTLCommandBuffer>`
    void* texture = nullptr;        ///< `id<MTLTexture>` of the drawable
    std::uint32_t width = 0;        ///< pixels
    std::uint32_t height = 0;
    double scale = 1.0;
};

/// Called on the render thread, with `rmlui_mutex()` held, around RmlUi's
/// update and render for one frame.
struct RendererHooks {
    std::function<void(const FrameTarget&)> begin_frame;
    std::function<void()> end_frame;
};

struct RmlUiSurfaceOptions {
    /// The RML document to load, resolved through the asset provider.
    std::string document;
    /// Font files to load, through the asset provider, before the document,
    /// so its text (a text field included) resolves a face when it is laid
    /// out. The first is also the fallback face.
    std::vector<std::string> font_faces{};
    double rate_cap_hz = 60.0;
    pulp::native::FrameThread thread = pulp::native::FrameThread::render;
    /// The `id<MTLDevice>` the caller's renderer was built on. The surface
    /// uses the same one, so the editor has one device. Null: the system
    /// default, which the renderer must then also use.
    void* device = nullptr;
    /// False for an editor that leaves parts of its window transparent; a
    /// renderer that clears to transparent shows black on an opaque layer.
    bool opaque = true;
};

class RmlUiSurface {
  public:
    /// `assets` and `renderer` must outlive the surface.
    RmlUiSurface(vellum::rmlui::AssetProvider& assets, Rml::RenderInterface& renderer,
                 RendererHooks hooks, RmlUiSurfaceOptions options);
    ~RmlUiSurface();
    RmlUiSurface(const RmlUiSurface&) = delete;
    RmlUiSurface& operator=(const RmlUiSurface&) = delete;

    /// Create the Metal surface in `parent` (`NSView*`), open the RmlUi
    /// context, load the document and start drawing. False, with nothing left
    /// behind, if any step fails.
    bool attach(void* parent);
    /// Stop drawing, close the RmlUi context and remove the surface view.
    void detach();
    /// The parent is gone: tear down without touching it.
    void invalidate();
    bool attached() const noexcept;

    /// Main-thread tick: visibility and size follow the window.
    void idle();
    /// Draw one more frame (after a change the caller made through RmlUi).
    void request_frame();

    /// Run `fn` with this surface's RmlUi context, holding `rmlui_mutex()` and
    /// with the instance in scope. Afterwards the surface holds the keyboard
    /// if `fn` focused a text field, and gives it back if `fn` blurred one.
    /// Returns false when there is no context. Main thread.
    bool with_context(const std::function<void(Rml::Context&)>& fn);

    pulp::native::MetalSurface* surface() noexcept;

    /// Frames rendered so far.
    std::uint64_t frames() const noexcept;

    /// RmlUiSurfaces alive in this binary.
    static std::size_t live() noexcept;

    struct Impl;

  private:
    std::unique_ptr<Impl> impl_;
};

/// The RmlUi key for a physical key, or `Rml::Input::KI_UNKNOWN` (0).
int rml_key_for(pulp::native::Key key) noexcept;
/// RmlUi modifier bits (`Rml::Input::KeyModifier`) for pulp-native ones.
int rml_modifiers_for(std::uint32_t modifiers) noexcept;
/// The pulp-native cursor for an RCSS `cursor` name; the arrow when unknown.
pulp::native::Cursor cursor_for_name(const std::string& name) noexcept;

} // namespace pulp::rmlui_native
