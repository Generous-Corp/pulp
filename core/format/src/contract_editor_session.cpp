#include <pulp/format/contract_editor_session.hpp>

#include <pulp/format/detail/main_thread_timer.hpp>

#include <pulp/format/processor.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/state/store.hpp>

#include <algorithm>
#include <atomic>
#include <utility>

namespace pulp::format {

class ContractEditorSession::Context final : public EditorContext {
  public:
    Context(ParamPort& port, EditorHostKind kind, HostHooks hooks)
        : port_(port), kind_(kind), hooks_(std::move(hooks)) {}

    ParamPort& params() override {
        return port_;
    }
    VisualPort* visuals() override {
        return nullptr;
    }
    bool request_resize(const EditorSize& size) override {
        if (owner_gone_.load() || !hooks_.request_resize || size.width == 0 || size.height == 0)
            return false;
        return hooks_.request_resize(size);
    }
    bool host_action(const HostActionRequest& request) override {
        if (owner_gone_.load())
            return false;
        return hooks_.host_action ? hooks_.host_action(request) : false;
    }
    void owner_gone() {
        owner_gone_.store(true);
    }
    EditorHostKind host_kind() const override {
        return kind_;
    }

  private:
    ParamPort& port_;
    EditorHostKind kind_;
    HostHooks hooks_;
    std::atomic<bool> owner_gone_{false};
};

ContractSessionRegistry::~ContractSessionRegistry() {
    close_all();
}

std::size_t ContractSessionRegistry::size() const {
    std::lock_guard lock(mutex_);
    std::size_t live = 0;
    for (const auto& weak : sessions_)
        if (!weak.expired())
            ++live;
    return live;
}

void ContractSessionRegistry::close_all() {
    std::vector<std::shared_ptr<ContractEditorSession>> live;
    {
        std::lock_guard lock(mutex_);
        for (const auto& weak : sessions_)
            if (auto session = weak.lock())
                live.push_back(std::move(session));
        sessions_.clear();
    }
    if (live.empty())
        return;
    if (detail::is_main_thread()) {
        for (const auto& session : live)
            session->close_for_owner();
        return;
    }
    // Off the main thread: never wait for it (a host's main thread may be
    // waiting for this one). Cut every session from the store now, while it
    // still exists, then close the editors on the main thread later. The
    // posted task holds the sessions, so the last reference drops there.
    for (const auto& session : live)
        session->disconnect_owner();
    // The posted task owns the only references, so this thread never drops
    // the last one (which would destroy an editor here).
    std::function<void()> close = [sessions = std::move(live)]() mutable {
        for (const auto& session : sessions)
            session->close_for_owner();
        sessions.clear();
    };
    if (!detail::post_to_main_thread(close)) {
        runtime::log_error("editor host: no main thread to close an editor on; closing it here");
        close();
    }
}

bool ContractEditorSession::register_with(ContractSessionRegistry& registry) {
    auto self = weak_from_this();
    if (self.expired()) {
        runtime::log_error("editor host: only a shared session can register for its owner");
        return false;
    }
    std::lock_guard lock(registry.mutex_);
    auto& sessions = registry.sessions_;
    sessions.erase(std::remove_if(sessions.begin(), sessions.end(),
                                  [](const auto& weak) { return weak.expired(); }),
                   sessions.end());
    sessions.push_back(std::move(self));
    return true;
}

void ContractEditorSession::disconnect_owner() {
    owner_gone_.store(true);
    if (port_)
        port_->disconnect();
    if (context_)
        context_->owner_gone();
}

std::unique_ptr<ContractEditorSession>
ContractEditorSession::create(Processor& processor, state::StateStore& store,
                              EditorFactory& factory, HostHooks hooks, IdleSource idle) {
    if (!factory.supports(EditorRole::editor))
        return nullptr;
    std::unique_ptr<ContractEditorSession> session(
        new ContractEditorSession(processor, store, std::move(hooks), idle));
    session->editor_ = factory.create(processor, *session->context_, EditorRole::editor);
    if (!session->editor_) {
        runtime::log_error("editor host: the editor factory built no editor");
        return nullptr;
    }
    return session;
}

ContractEditorSession::ContractEditorSession(Processor& processor, state::StateStore& store,
                                             HostHooks hooks, IdleSource idle)
    : idle_source_(idle), port_(std::make_unique<StateStoreParamPort>(store)),
      context_(std::make_unique<Context>(*port_, processor.editor_host_kind(), std::move(hooks))) {}

ContractEditorSession::~ContractEditorSession() {
    stop_idle();
    // Destroyed while attached: the host tore its window down without an
    // orderly detach.
    if (editor_ && attached_)
        editor_->invalidate();
    editor_.reset();
    context_.reset();
    port_.reset();
}

void ContractEditorSession::start_idle() {
    stop_idle();
    if (idle_source_ != IdleSource::self_paced)
        return;
    if (!idle_timer_.start(kIdleIntervalMs, [this] { tick(); }))
        runtime::log_error("editor host: no main-thread timer for the editor's idle tick");
}

void ContractEditorSession::stop_idle() {
    idle_timer_.stop();
}

void ContractEditorSession::use_self_paced_idle() {
    idle_source_ = IdleSource::self_paced;
    if (attached_)
        start_idle();
}

bool ContractEditorSession::attach(const NativeParent& parent) {
    if (!editor_ || attached_ || parent.kind == NativeParentKind::none)
        return false;
    if (parent.handle == nullptr && parent.window_id == 0)
        return false;
    if (!editor_->attach(parent)) {
        runtime::log_error("editor host: the editor did not attach to the host window");
        return false;
    }
    attached_ = true;
    start_idle();
    return true;
}

void ContractEditorSession::detach() {
    stop_idle();
    // Closing the window mid-drag must still balance every begin.
    if (port_)
        port_->release_all();
    if (editor_ && attached_)
        editor_->detach();
    attached_ = false;
}

void ContractEditorSession::close_for_owner() {
    // detach() stops the tick, releases every gesture and detaches the editor
    // from the window the host still owns.
    detach();
    destroy_editor();
}

void ContractEditorSession::invalidate() {
    stop_idle();
    if (editor_)
        editor_->invalidate();
    attached_ = false;
    destroy_editor();
}

void ContractEditorSession::destroy_editor() {
    editor_.reset();
    context_.reset();
    // Destroying the port releases every gesture it still holds.
    port_.reset();
}

void ContractEditorSession::tick() {
    // Once the owner is gone, draw nothing until the main thread closes the
    // editor: its ports are cut, and a frame would show nothing true.
    if (editor_ && attached_ && !owner_gone_.load())
        editor_->idle();
}

EditorSize ContractEditorSession::size() const {
    if (!editor_)
        return {};
    auto size = editor_->size();
    if (size.width == 0 || size.height == 0) {
        const auto hints = editor_->size_hints();
        size.width = hints.preferred_width;
        size.height = hints.preferred_height;
    }
    return size;
}

ViewSize ContractEditorSession::size_hints() const {
    return editor_ ? editor_->size_hints() : ViewSize{};
}

bool ContractEditorSession::set_size(std::uint32_t width, std::uint32_t height) {
    if (!editor_ || width == 0 || height == 0)
        return false;
    EditorSize size = editor_->size();
    size.width = width;
    size.height = height;
    return editor_->set_size(size);
}

bool ContractEditorSession::set_scale(double scale) {
    if (!editor_ || !(scale > 0.0))
        return false;
    // The editor keeps the scale in its size, so a later set_size() carries it.
    EditorSize size = this->size();
    size.scale = scale;
    return editor_->set_size(size);
}

bool ContractEditorSession::resizable() const {
    if (!editor_)
        return false;
    const auto h = editor_->size_hints();
    const bool fixed = h.min_width > 0 && h.min_width == h.max_width && h.min_height > 0 &&
                       h.min_height == h.max_height;
    return !fixed;
}

bool ContractEditorSession::constrain(std::uint32_t& width, std::uint32_t& height) const {
    if (!editor_ || width == 0 || height == 0)
        return false;
    EditorSize proposed = editor_->size();
    proposed.width = width;
    proposed.height = height;
    auto adjusted = editor_->adjust_size(proposed);
    const auto h = editor_->size_hints();
    auto clamp = [](std::uint32_t v, std::uint32_t lo, std::uint32_t hi) {
        if (lo > 0 && v < lo)
            v = lo;
        if (hi > 0 && v > hi)
            v = hi;
        return v;
    };
    adjusted.width = clamp(adjusted.width, h.min_width, h.max_width);
    adjusted.height = clamp(adjusted.height, h.min_height, h.max_height);
    if (adjusted.width == 0 || adjusted.height == 0)
        return false;
    width = adjusted.width;
    height = adjusted.height;
    return true;
}

KeyDisposition ContractEditorSession::offer_key(const EditorKeyEvent& event) {
    if (!editor_ || !attached_)
        return KeyDisposition::forward;
    return editor_->offer_key(event);
}

void ContractEditorSession::focus_changed(bool focused) {
    if (editor_)
        editor_->focus_changed(focused);
}

void ContractEditorSession::visibility_changed(bool visible) {
    if (editor_)
        editor_->visibility_changed(visible);
}

} // namespace pulp::format
