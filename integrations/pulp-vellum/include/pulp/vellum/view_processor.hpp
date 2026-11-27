#pragma once

/// @file
/// `pulp::vellum::ViewProcessor<>`: the `create_view()` editor hooks, for a
/// plug-in built against an SDK whose UI comes from Vellum.
///
/// In that SDK `format::Processor` declares no view hook (its legacy slots are
/// reserved, see `PULP_FORMAT_LEGACY_VIEW` in processor.hpp). A plug-in that
/// returned a view tree keeps its code by deriving from this class instead of
/// `format::Processor`:
///
/// ```cpp
/// class MyPlugin : public pulp::vellum::ViewProcessor<> {
///   public:
///     std::unique_ptr<::vellum::view::View> create_view() override;
///     void on_view_opened(::vellum::view::View& view) override;
/// };
/// ```
///
/// `editor_factory()` returns a `ViewEditorFactory` over these hooks, so the
/// editor opens through the editor-host contract in every format adapter, with
/// the ViewBridge protocol: `on_view_opened` after a native attach,
/// `on_view_resized` while attached, `on_view_closed` once per attach, and no
/// call once the instance is gone (see view_editor.hpp). This override wins
/// over a `pulp_add_plugin(... UI <package>)` editor.
///
/// `create_view()` builds the main editor. `create_view(role)` builds any role
/// the factory offers (editor and inspector); its default calls
/// `create_view()` for the editor and returns null otherwise, so a second
/// window is opt-in. Every call is on the host's main thread.

#include <pulp/format/editor_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/vellum/view_editor.hpp>

#include <vellum/view/view.hpp>

#include <cstdint>
#include <memory>

#if PULP_FORMAT_LEGACY_VIEW
#error "pulp/vellum/view_processor.hpp is for an SDK built with PULP_FORMAT_LEGACY_VIEW=0; \
with Pulp's own view layer, override format::Processor::create_view() directly"
#endif

namespace pulp::vellum {

template <class Base = format::Processor> class ViewProcessor : public Base {
  public:
    using Base::Base;

    /// The main editor's view tree, or null for no editor.
    virtual std::unique_ptr<::vellum::view::View> create_view() {
        return nullptr;
    }

    /// The view tree for `role`. Default: `create_view()` for the editor, null
    /// for any other role.
    virtual std::unique_ptr<::vellum::view::View> create_view(format::EditorRole role) {
        return role == format::EditorRole::editor ? create_view() : nullptr;
    }

    /// After the view attached to a native window.
    virtual void on_view_opened(::vellum::view::View& /*view*/) {}

    /// Before the view's window goes away, once per attach.
    virtual void on_view_closed(::vellum::view::View& /*view*/) {}

    /// The editor took a new size, in logical pixels, while attached.
    virtual void on_view_resized(::vellum::view::View& /*view*/, std::uint32_t /*w*/,
                                 std::uint32_t /*h*/) {}

    format::EditorFactory* editor_factory() override {
        return &factory();
    }

  private:
    // One factory per Base. It holds no processor: each call reaches the
    // processor the host passes to create(), which is the one whose
    // editor_factory() returned it.
    static ViewEditorFactory& factory() {
        static ViewEditorFactory instance(
            [](format::Processor& processor,
               format::EditorRole role) -> std::unique_ptr<::vellum::view::View> {
                auto* self = dynamic_cast<ViewProcessor*>(&processor);
                return self != nullptr ? self->create_view(role) : nullptr;
            },
            ViewEditorHooks{
                .opened =
                    [](format::Processor& processor, ::vellum::view::View& view) {
                        if (auto* self = dynamic_cast<ViewProcessor*>(&processor))
                            self->on_view_opened(view);
                    },
                .resized =
                    [](format::Processor& processor, ::vellum::view::View& view, std::uint32_t w,
                       std::uint32_t h) {
                        if (auto* self = dynamic_cast<ViewProcessor*>(&processor))
                            self->on_view_resized(view, w, h);
                    },
                .closing =
                    [](format::Processor& processor, ::vellum::view::View& view) {
                        if (auto* self = dynamic_cast<ViewProcessor*>(&processor))
                            self->on_view_closed(view);
                    },
            });
        return instance;
    }
};

} // namespace pulp::vellum
