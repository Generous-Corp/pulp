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
/// `editor_factory()` returns this processor's factory over these hooks, so the
/// editor opens through the editor-host contract in every format adapter, with
/// the ViewBridge protocol: `on_view_opened` after a native attach,
/// `on_view_resized` while attached, `on_view_closed` once per attach, and no
/// call once the instance is gone (see view_editor.hpp). This override wins
/// over a `pulp_add_plugin(... UI <package>)` editor.
///
/// `create_view()` builds the main editor. `create_view(role)` builds any role
/// the factory offers; its default calls `create_view()` for the editor, builds
/// the settings editor from `settings_sections()`, and returns null otherwise.
/// `has_view(role)` says which roles the factory offers, without building
/// anything: the editor by default, the settings editor when `has_settings()`
/// is true, so a second window is opt-in. A plug-in whose `create_view()` can
/// return null overrides `has_view`. Hosts ask it whenever they query an
/// editor, so it and `has_settings()` must be cheap. Every call is on the
/// host's main thread.
///
/// `settings_sections()` is the Vellum form of Processor's legacy settings
/// tabs. A host that offers the settings role (the standalone's
/// "<name> Settings…" window) shows them, one tab per section, or the single
/// section's view alone.
///
/// Availability: this needs an SDK built with `PULP_FORMAT_LEGACY_VIEW=0`, which
/// is the SDK that serves its UI from Vellum. Until a plug-in builds against
/// one, it keeps overriding `format::Processor::create_view()`; the two cannot
/// coexist in one build, because they differ only in return type. The changes
/// to make at the switch are listed in docs/guides/editor-host-contract.md.

#include <pulp/format/editor_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/vellum/settings_view.hpp>
#include <pulp/vellum/view_editor.hpp>

#include <vellum/view/view.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if PULP_FORMAT_LEGACY_VIEW
#error "pulp/vellum/view_processor.hpp needs a Pulp SDK built with PULP_FORMAT_LEGACY_VIEW=0 \
(its UI served from Vellum). This SDK keeps Pulp's own view layer: override \
format::Processor::create_view() instead, and move to ViewProcessor<> when you \
build against a Vellum-UI SDK (docs/guides/editor-host-contract.md)."
#endif

namespace pulp::vellum {


template <class Base = format::Processor> class ViewProcessor : public Base {
  public:
    using Base::Base;

    /// The main editor's view tree.
    virtual std::unique_ptr<::vellum::view::View> create_view() = 0;

    /// The view tree for `role`. Default: `create_view()` for the editor, the
    /// `settings_sections()` for settings, null for any other role.
    virtual std::unique_ptr<::vellum::view::View> create_view(format::EditorRole role) {
        if (role == format::EditorRole::editor)
            return create_view();
        if (role == format::EditorRole::settings)
            return make_settings_view(settings_sections());
        return nullptr;
    }

    /// The settings tabs, built each time the settings editor opens. Default:
    /// none.
    virtual std::vector<ViewSettingsSection> settings_sections() {
        return {};
    }

    /// Whether `settings_sections()` returns a section with a view, answered
    /// without building one. Override it with `settings_sections()`. Default:
    /// false, so no settings editor.
    virtual bool has_settings() const {
        return false;
    }

    /// Whether `create_view(role)` builds a view, answered without building one.
    /// Default: the editor, and the settings editor when `has_settings()`.
    virtual bool has_view(format::EditorRole role) const {
        return role == format::EditorRole::editor ||
               (role == format::EditorRole::settings && has_settings());
    }

    /// After the view attached to a native window.
    virtual void on_view_opened(::vellum::view::View& /*view*/) {}

    /// Before the view's window goes away, once per attach.
    virtual void on_view_closed(::vellum::view::View& /*view*/) {}

    /// The editor took a new size, in logical pixels, while attached.
    virtual void on_view_resized(::vellum::view::View& /*view*/, std::uint32_t /*w*/,
                                 std::uint32_t /*h*/) {}

    format::EditorFactory* editor_factory() override {
        return &factory_;
    }

  private:
    // This processor's factory: offers what has_view() says, and builds
    // through the shared builder below. Owned by the processor, as the
    // contract asks.
    class Factory final : public format::EditorFactory {
      public:
        explicit Factory(ViewProcessor& owner) : owner_(owner) {}
        bool supports(format::EditorRole role) const override {
            return owner_.has_view(role);
        }
        std::unique_ptr<format::Editor> create(format::Processor& processor,
                                               format::EditorContext& context,
                                               format::EditorRole role) override {
            return supports(role) ? builder().create(processor, context, role) : nullptr;
        }

      private:
        ViewProcessor& owner_;
    };

    // One building factory per Base. It holds no processor: each call reaches
    // the processor the host passes to create(), the one whose factory asked.
    static ViewEditorFactory& builder() {
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
            },
            {format::EditorRole::editor, format::EditorRole::inspector,
             format::EditorRole::settings});
        return instance;
    }

    Factory factory_{*this};
};

} // namespace pulp::vellum
