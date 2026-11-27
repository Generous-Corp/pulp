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
/// The factory holds no per-plug-in state: everything reaches the plug-in
/// through the `Processor&` that `create()` receives, so one function-local
/// factory serves every processor in an image.
///
/// Main thread only, like every editor-host call.

#include <pulp/format/editor_host.hpp>
#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/vellum/editor.hpp>

#include <vellum/view/view.hpp>

#include <cstdint>
#include <functional>
#include <initializer_list>
#include <memory>
#include <utility>
#include <vector>

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

namespace detail {

// What ViewEditorFactory::create() hands to the root it constructs. The root
// is built synchronously inside create(), on the main thread, so a
// thread-local slot set around that construction reaches exactly that root.
struct ViewEditorRequest {
    std::unique_ptr<::vellum::view::View> view;
    const ViewEditorHooks* hooks = nullptr;
};

inline ViewEditorRequest*& pending_view_editor_request() noexcept {
    thread_local ViewEditorRequest* request = nullptr;
    return request;
}

// The root Editor<> hosts: holds the built view as its only child, fills it to
// its own bounds and turns the editor's attach edges into the hooks.
class ViewEditorRoot final : public ::vellum::view::View {
  public:
    ViewEditorRoot(EditorEnv& env, format::Processor& processor)
        : env_(env), processor_(processor) {
        auto* request = pending_view_editor_request();
        if (request == nullptr)
            return;
        hooks_ = request->hooks;
        auto* view = request->view.get();
        // add_child() returns null once it has adopted the child.
        if (view != nullptr && add_child(std::move(request->view)) == nullptr)
            child_ = view;
    }

    format::ViewSize editor_view_size() const {
        return processor_reachable() ? processor_.view_size() : format::ViewSize{};
    }

    void editor_opened() {
        open_ = true;
        // The size at open is the starting point; `resized` reports changes.
        last_width_ = static_cast<std::uint32_t>(bounds().width);
        last_height_ = static_cast<std::uint32_t>(bounds().height);
        if (child_ != nullptr && hooks_ != nullptr && hooks_->opened && processor_reachable())
            hooks_->opened(processor_, *child_);
    }

    void editor_closing() {
        if (!std::exchange(open_, false))
            return;
        if (!processor_reachable())
            return;
        processor_.state().release_open_gestures();
        if (child_ != nullptr && hooks_ != nullptr && hooks_->closing)
            hooks_->closing(processor_, *child_);
    }

    void on_resized() override {
        notify_resized();
    }

    // The plug-in's view is the whole editor, as it was under ViewBridge, so it
    // takes the root's full bounds instead of a flex child's measured size.
    void layout_children() override {
        if (child_ == nullptr)
            return;
        child_->set_bounds(local_bounds());
        child_->layout_children();
    }

    ::vellum::view::View* hosted_view() const noexcept {
        return child_;
    }

  private:
    // A host cuts the editor's parameter port from the store before the
    // processor goes away. Pulp's hosts use StateStoreParamPort; a host with
    // another port gives no such signal and is trusted to close first.
    bool processor_reachable() const {
        auto* port = dynamic_cast<const format::StateStoreParamPort*>(&env_.context.params());
        return port == nullptr || port->connected();
    }

    void notify_resized() {
        if (!open_ || child_ == nullptr || hooks_ == nullptr || !hooks_->resized ||
            !processor_reachable())
            return;
        const auto b = bounds();
        const auto w = static_cast<std::uint32_t>(b.width);
        const auto h = static_cast<std::uint32_t>(b.height);
        if (w == last_width_ && h == last_height_)
            return;
        last_width_ = w;
        last_height_ = h;
        hooks_->resized(processor_, *child_, w, h);
    }

    EditorEnv& env_;
    format::Processor& processor_;
    const ViewEditorHooks* hooks_ = nullptr;
    ::vellum::view::View* child_ = nullptr;
    bool open_ = false;
    std::uint32_t last_width_ = 0;
    std::uint32_t last_height_ = 0;
};

} // namespace detail

/// A `format::EditorFactory` that hosts the view `make` builds.
class ViewEditorFactory final : public format::EditorFactory {
  public:
    explicit ViewEditorFactory(ViewFactory make, ViewEditorHooks hooks = {},
                               std::initializer_list<format::EditorRole> roles =
                                   {format::EditorRole::editor, format::EditorRole::inspector})
        : make_(std::move(make)), hooks_(std::move(hooks)), roles_(roles) {}

    bool supports(format::EditorRole role) const override {
        for (auto r : roles_)
            if (r == role)
                return true;
        return false;
    }

    std::unique_ptr<format::Editor> create(format::Processor& processor,
                                           format::EditorContext& context,
                                           format::EditorRole role) override {
        if (!supports(role) || !make_)
            return nullptr;
        detail::ViewEditorRequest request{make_(processor, role), &hooks_};
        if (request.view == nullptr)
            return nullptr;
        auto*& slot = detail::pending_view_editor_request();
        auto* previous = std::exchange(slot, &request);
        struct Restore {
            detail::ViewEditorRequest*& slot;
            detail::ViewEditorRequest* previous;
            ~Restore() {
                slot = previous;
            }
        } restore{slot, previous};
        return std::make_unique<Editor<detail::ViewEditorRoot>>(context, processor);
    }

  private:
    ViewFactory make_;
    ViewEditorHooks hooks_;
    std::vector<format::EditorRole> roles_;
};

} // namespace pulp::vellum
