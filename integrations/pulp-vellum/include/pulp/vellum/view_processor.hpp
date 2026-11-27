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
/// `editor_factory()` returns a factory over these hooks, so the
/// editor opens through the editor-host contract in every format adapter, with
/// the ViewBridge protocol: `on_view_opened` after a native attach,
/// `on_view_resized` while attached, `on_view_closed` once per attach, and no
/// call once the instance is gone (see view_editor.hpp). This override wins
/// over a `pulp_add_plugin(... UI <package>)` editor.
///
/// `create_view()` builds the main editor. `create_view(role)` builds any role
/// the factory offers (editor, inspector and settings); its default calls
/// `create_view()` for the editor, builds the settings editor from
/// `settings_sections()`, and returns null otherwise, so a second window is
/// opt-in. Every call is on the host's main thread.
///
/// `settings_sections()` is the Vellum form of Processor's legacy settings
/// tabs. A host that offers the settings role (the standalone's
/// "<name> Settings…" window) shows them, one tab per section, or the single
/// section's view alone.

#include <pulp/format/editor_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/vellum/view_editor.hpp>

#include <vellum/view/ui_components.hpp>
#include <vellum/view/view.hpp>

#include <cstdint>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#if PULP_FORMAT_LEGACY_VIEW
#error "pulp/vellum/view_processor.hpp is for an SDK built with PULP_FORMAT_LEGACY_VIEW=0; \
with Pulp's own view layer, override format::Processor::create_view() directly"
#endif

namespace pulp::vellum {

/// One settings tab a ViewProcessor contributes.
struct ViewSettingsSection {
    std::string title;
    std::unique_ptr<::vellum::view::View> view;
};

template <class Base = format::Processor> class ViewProcessor : public Base {
  public:
    using Base::Base;

    /// The main editor's view tree, or null for no editor.
    virtual std::unique_ptr<::vellum::view::View> create_view() {
        return nullptr;
    }

    /// The view tree for `role`. Default: `create_view()` for the editor, the
    /// `settings_sections()` for settings, null for any other role.
    virtual std::unique_ptr<::vellum::view::View> create_view(format::EditorRole role) {
        if (role == format::EditorRole::editor)
            return create_view();
        if (role == format::EditorRole::settings)
            return settings_view(settings_sections());
        return nullptr;
    }

    /// The settings tabs, built each time the settings editor opens. Default:
    /// none, so no settings editor.
    virtual std::vector<ViewSettingsSection> settings_sections() {
        return {};
    }

    /// After the view attached to a native window.
    virtual void on_view_opened(::vellum::view::View& /*view*/) {}

    /// Before the view's window goes away, once per attach.
    virtual void on_view_closed(::vellum::view::View& /*view*/) {}

    /// The editor took a new size, in logical pixels, while attached.
    virtual void on_view_resized(::vellum::view::View& /*view*/, std::uint32_t /*w*/,
                                 std::uint32_t /*h*/) {}

    /// This processor's editor factory. It offers the settings role only while
    /// `settings_sections()` returns a section with a view, so a host shows no
    /// settings item for a plug-in that has none.
    format::EditorFactory* editor_factory() override {
        return &advertised_;
    }

  private:
    // Answers supports() for its own processor, then builds through the
    // shared factory below. Owned by the processor, as the contract asks.
    class AdvertisedFactory final : public format::EditorFactory {
      public:
        explicit AdvertisedFactory(ViewProcessor& owner) : owner_(owner) {}

        bool supports(format::EditorRole role) const override {
            if (role == format::EditorRole::settings) {
                for (const auto& section : owner_.settings_sections())
                    if (section.view != nullptr)
                        return true;
                return false;
            }
            return factory().supports(role);
        }

        std::unique_ptr<format::Editor> create(format::Processor& processor,
                                               format::EditorContext& context,
                                               format::EditorRole role) override {
            return supports(role) ? factory().create(processor, context, role) : nullptr;
        }

        void prewarm() override {
            factory().prewarm();
        }

      private:
        ViewProcessor& owner_;
    };

    static std::unique_ptr<::vellum::view::View>
    settings_view(std::vector<ViewSettingsSection> sections) {
        std::erase_if(sections, [](const ViewSettingsSection& s) { return s.view == nullptr; });
        if (sections.empty())
            return nullptr;
        if (sections.size() == 1)
            return std::move(sections.front().view);
        auto tabs = std::make_unique<::vellum::view::TabPanel>();
        for (auto& section : sections)
            tabs->add_tab(std::move(section.title), std::move(section.view));
        return tabs;
    }

    // One building factory per Base. It holds no processor: each call reaches
    // the processor the host passes to create(), which is the one whose
    // editor_factory() returned this processor's AdvertisedFactory.
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
            },
            {format::EditorRole::editor, format::EditorRole::inspector,
             format::EditorRole::settings});
        return instance;
    }

    AdvertisedFactory advertised_{*this};
};

} // namespace pulp::vellum
