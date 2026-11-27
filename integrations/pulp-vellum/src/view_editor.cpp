#include <pulp/vellum/view_editor.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/state/store.hpp>
#include <pulp/vellum/editor.hpp>

#include <vellum/view/view.hpp>

#include <utility>

namespace pulp::vellum {
namespace {

// What ViewEditorFactory::create() hands to the root it constructs. The root
// is built synchronously inside create(), on the main thread, so a
// thread-local slot set around that construction reaches exactly that root.
struct ViewEditorRequest {
    std::unique_ptr<::vellum::view::View> view;
    std::shared_ptr<const ViewEditorHooks> hooks;
};

ViewEditorRequest*& pending_request() noexcept {
    thread_local ViewEditorRequest* request = nullptr;
    return request;
}

// Every EditorIdleTarget at or below `view`, depth first.
void tick_idle_targets(::vellum::view::View& view) {
    if (auto* target = dynamic_cast<EditorIdleTarget*>(&view))
        target->editor_idle();
    for (std::size_t i = 0; i < view.child_count(); ++i)
        tick_idle_targets(*view.child_at(i));
}

// The root Editor<> hosts: holds the built view as its only child, fills it to
// its own bounds and turns the editor's attach edges into the hooks.
class ViewEditorRoot final : public ::vellum::view::View {
  public:
    ViewEditorRoot(EditorEnv& env, format::Processor& processor)
        : env_(env), processor_(processor) {
        auto* request = pending_request();
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
        if (child_ != nullptr && hooks_ && hooks_->opened && processor_reachable())
            hooks_->opened(processor_, *child_);
    }

    void editor_closing() {
        if (!std::exchange(open_, false))
            return;
        if (!processor_reachable())
            return;
        processor_.state().release_open_gestures();
        if (child_ != nullptr && hooks_ && hooks_->closing)
            hooks_->closing(processor_, *child_);
    }

    void editor_idle() {
        if (open_ && child_ != nullptr)
            tick_idle_targets(*child_);
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

  private:
    // A host cuts the editor's parameter port from the store before the
    // processor goes away. Pulp's hosts use StateStoreParamPort; a host with
    // another port gives no such signal and is trusted to close first.
    bool processor_reachable() const {
        auto* port = dynamic_cast<const format::StateStoreParamPort*>(&env_.context.params());
        return port == nullptr || port->connected();
    }

    void notify_resized() {
        if (!open_ || child_ == nullptr || !hooks_ || !hooks_->resized || !processor_reachable())
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
    std::shared_ptr<const ViewEditorHooks> hooks_;
    ::vellum::view::View* child_ = nullptr;
    bool open_ = false;
    std::uint32_t last_width_ = 0;
    std::uint32_t last_height_ = 0;
};

} // namespace

ViewEditorFactory::ViewEditorFactory(ViewFactory make, ViewEditorHooks hooks,
                                     std::initializer_list<format::EditorRole> roles)
    : make_(std::move(make)), hooks_(std::make_shared<const ViewEditorHooks>(std::move(hooks))),
      roles_(roles) {}

ViewEditorFactory::~ViewEditorFactory() = default;

bool ViewEditorFactory::supports(format::EditorRole role) const {
    for (auto r : roles_)
        if (r == role)
            return true;
    return false;
}

std::unique_ptr<format::Editor> ViewEditorFactory::create(format::Processor& processor,
                                                          format::EditorContext& context,
                                                          format::EditorRole role) {
    if (!supports(role) || !make_)
        return nullptr;
    ViewEditorRequest request{make_(processor, role), hooks_};
    if (request.view == nullptr)
        return nullptr;
    auto*& slot = pending_request();
    auto* previous = std::exchange(slot, &request);
    struct Restore {
        ViewEditorRequest*& slot;
        ViewEditorRequest* previous;
        ~Restore() {
            slot = previous;
        }
    } restore{slot, previous};
    return std::make_unique<Editor<ViewEditorRoot>>(context, processor);
}

} // namespace pulp::vellum
