#pragma once

/// @file rmlui_surface.hpp
/// RmlUi on a pulp-native Metal surface: the glue between Vellum's open RmlUi
/// entry point (`VellumRmlUiNative::Native`) and `pulp::native`.
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
/// `rmlui_mutex()`; a frame holds it for its update and render. Members other
/// than `with_context()` run on the main thread.
///
/// The main thread does not wait for a frame to deliver pointer or wheel
/// input or to run `idle()`. When the render thread holds the lock, pointer
/// and wheel events are queued (consecutive moves and wheel deltas merge) and
/// applied, in order and on the main thread, after the
/// frame ends. Their listeners therefore run on the main thread, a moment
/// later than the event. `with_context()`, key and text input apply anything
/// queued first, so they always see earlier events' effects; they, and
/// `flush_input()`, do wait for a frame.

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

/// Whether this build tells RmlUi the backing scale as its density ratio, so a
/// length in `dp` is one point. False when the Vellum RmlUi SDK it was built
/// against has no `set_density_ratio`: `dp` then equals `px`, and a document
/// sized in `dp` renders at 1/scale of its intended size on a 2x display.
/// The first `attach()` with this false reports it through the diagnostic sink, once per process (setting a new sink re-arms it).
bool density_ratio_supported() noexcept;

/// Where the adapter's diagnostics go. Default: standard error. Pass an empty
/// function to restore it. Call from the main thread, before attaching.
void set_diagnostic_sink(std::function<void(const std::string&)> sink);

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
    /// True while an RmlUi text field holds the keyboard. Main thread.
    bool text_field_focused() const noexcept;
    /// A host offers `event` before acting on it itself (a transport shortcut
    /// on Space, say). Returns true when the surface took it: a text field has
    /// focus, and RmlUi consumed the key or the key produced text, which is
    /// delivered to the field. Returns false, without touching RmlUi, when no
    /// text field has focus, so the host keeps its shortcuts. Repeats and
    /// releases keep the initial press disposition even if focus changes while
    /// the key is held. Pointer
    /// input queued behind a frame is applied first, so a click on a field
    /// that has not landed yet still decides where the key goes. Main thread.
    bool offer_key(const pulp::native::KeyEvent& event);
    /// The editor's window gained or lost keyboard focus. Losing it blurs a
    /// focused text field (the caret stops showing and the host's keyboard is
    /// handed back); gaining it focuses that field again, found by its `id`
    /// attribute, so a field without one is not restored. Main thread.
    void set_window_focused(bool focused);
    /// The density ratio RmlUi was given: the backing scale when
    /// `density_ratio_supported()`, otherwise 1, and 1 while detached. `px` lengths are always
    /// device pixels; `dp` lengths are `density_ratio()` device pixels. Main
    /// thread.
    double density_ratio() const noexcept;
    /// Apply pointer and wheel events that were queued behind a frame, now,
    /// waiting for the frame if one is in flight. Main thread.
    void flush_input();
    /// Pointer and wheel events queued behind a frame so far (merged events
    /// count once each as they arrived). For tests and diagnostics.
    std::uint64_t queued_inputs() const noexcept;
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
