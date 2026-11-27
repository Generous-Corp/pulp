#pragma once

/// @file
/// An editor-host `EditorFactory` over a function that builds a ready
/// `vellum::view::View` tree, the shape of a legacy `Processor::create_view()`.
///
/// `ViewEditorFactory` lets a processor whose editor is "return a view tree"
/// open through the editor-host contract (`pulp/format/editor_host.hpp`) that
/// every format adapter already prefers, without naming a Pulp view type. The
/// view is hosted by `pulp::vellum::Editor`, so size hints, design viewports,
/// content-first presentation, keys and capture behave as for any Vellum
/// editor.
///
/// Lifecycle, matching the ViewBridge protocol the hooks come from:
/// - `make` runs inside `EditorFactory::create()`, before any window exists. A
///   null view means the processor has no editor for that role, and `create()`
///   returns null.
/// - `opened` runs after the editor attaches to a real native parent, never for
///   an offscreen (`NativeParentKind::none`) attach and never after a failed
///   attach.
/// - `resized` runs for every new size while attached, host resizes and
///   editor-requested resizes alike, in logical pixels.
/// - `closing` runs once per successful attach, on detach or invalidate. When
///   the processor is still reachable the store's open gestures are released
///   first, because a view that writes the store directly bypasses the
///   editor's parameter port and the port's own release.
/// - Once the editor's parameter port is disconnected (its plug-in instance
///   went away), no hook runs and the processor is not touched again.
///
/// Size hints come from `Processor::view_size()`, as ViewBridge reads them.
///
/// Each editor copies the hooks, so it may outlive the factory that made it.
///
/// A hosted view (or any view below it) that implements `EditorIdleTarget`
/// gets `editor_idle()` on every editor idle tick while attached, so a view
/// that mirrors outside state (an update check, a meter) stays live.
///
/// The factory holds no per-plug-in state: everything reaches the plug-in
/// through the `Processor&` that `create()` receives, so one function-local
/// factory serves every processor in an image.
///
/// Main thread only, like every editor-host call.

#include <pulp/format/editor_host.hpp>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <vector>

namespace vellum::view {
class View;
}

namespace pulp::format {
class Processor;
}

namespace pulp::vellum {

/// Builds the view tree for one editor window, or null for no editor.
using ViewFactory =
    std::function<std::unique_ptr<::vellum::view::View>(format::Processor&, format::EditorRole)>;

/// Processor-side callbacks around a hosted view. Each may be empty.
struct ViewEditorHooks {
    std::function<void(format::Processor&, ::vellum::view::View&)> opened;
    std::function<void(format::Processor&, ::vellum::view::View&, std::uint32_t, std::uint32_t)>
        resized;
    std::function<void(format::Processor&, ::vellum::view::View&)> closing;
};

/// A view that wants the editor's idle tick. Mix it into a `vellum::view::View`
/// subclass anywhere in the hosted tree.
class EditorIdleTarget {
  public:
    virtual void editor_idle() = 0;

  protected:
    ~EditorIdleTarget() = default;
};

/// A `format::EditorFactory` that hosts the view `make` builds.
class ViewEditorFactory final : public format::EditorFactory {
  public:
    explicit ViewEditorFactory(ViewFactory make, ViewEditorHooks hooks = {},
                               std::initializer_list<format::EditorRole> roles = {
                                   format::EditorRole::editor, format::EditorRole::inspector});
    ~ViewEditorFactory() override;

    bool supports(format::EditorRole role) const override;
    std::unique_ptr<format::Editor> create(format::Processor& processor,
                                           format::EditorContext& context,
                                           format::EditorRole role) override;

  private:
    ViewFactory make_;
    ViewEditorHooks hooks_;
    std::vector<format::EditorRole> roles_;
};

} // namespace pulp::vellum
