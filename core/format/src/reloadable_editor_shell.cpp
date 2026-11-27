#include <pulp/format/reload/reloadable_editor_shell.hpp>

#include <pulp/format/editor_host.hpp>
#include <pulp/format/reload/reloadable_shell_core.hpp>

#include <optional>
#include <utility>

namespace pulp::format::reload {
namespace {

class EditorShell;

// One editor of the shell: wraps the active logic's editor for one window.
class ForwardingEditor final : public Editor {
  public:
    ForwardingEditor(EditorShell& shell, EditorContext& context, EditorRole role);
    ~ForwardingEditor() override {
        drop();
    }

    bool has_logic_editor() const {
        return inner_ != nullptr;
    }

    bool supports_parent(NativeParentKind kind) const override {
        return inner_ != nullptr && inner_->supports_parent(kind);
    }
    bool attach(const NativeParent& parent) override {
        if (invalidated_ || inner_ == nullptr || !inner_->attach(parent))
            return false;
        parent_ = parent;
        return true;
    }
    void detach() override {
        parent_.reset();
        if (inner_ != nullptr)
            inner_->detach();
    }
    void invalidate() override {
        // The host's window is gone and the editor is about to be destroyed:
        // nothing may build again, whatever the reload generation does.
        invalidated_ = true;
        parent_.reset();
        if (inner_ != nullptr)
            inner_->invalidate();
        drop();
    }
    EditorSize size() const override {
        return inner_ != nullptr ? inner_->size() : size_;
    }
    bool set_size(const EditorSize& size) override {
        if (inner_ == nullptr || !inner_->set_size(size))
            return false;
        size_ = inner_->size();
        return true;
    }
    ViewSize size_hints() const override {
        return inner_ != nullptr ? inner_->size_hints() : Editor::size_hints();
    }
    EditorSize adjust_size(const EditorSize& proposed) const override {
        return inner_ != nullptr ? inner_->adjust_size(proposed) : proposed;
    }
    void idle() override;
    void visibility_changed(bool visible) override {
        if (inner_ != nullptr)
            inner_->visibility_changed(visible);
    }
    void focus_changed(bool focused) override {
        if (inner_ != nullptr)
            inner_->focus_changed(focused);
    }
    KeyDisposition offer_key(const EditorKeyEvent& event) override {
        return inner_ != nullptr ? inner_->offer_key(event) : KeyDisposition::forward;
    }
    void parameter_highlight(state::ParamID id, bool highlighted,
                             std::uint32_t color_rgb) override {
        if (inner_ != nullptr)
            inner_->parameter_highlight(id, highlighted, color_rgb);
    }
    EditorAutomation* automation() override {
        return inner_ != nullptr ? inner_->automation() : nullptr;
    }

  private:
    void build();
    void drop();
    void rebuild();

    EditorShell& shell_;
    EditorContext& context_;
    EditorRole role_;
    std::unique_ptr<Editor> inner_;
    Processor* held_ = nullptr;
    std::optional<NativeParent> parent_;
    EditorSize size_{0, 0, 1.0};
    std::uint64_t generation_ = 0;
    bool invalidated_ = false;
};

class EditorShell final : public ReloadableShellCore, public ReloadableEditorShellControl {
  public:
    explicit EditorShell(std::string logic_path) : ReloadableShellCore(std::move(logic_path)) {}

    EditorFactory* editor_factory() override {
        return &factory_;
    }

    bool reload_now(std::string* error) override {
        const auto outcome = ReloadableShellCore::reload_now();
        if (!outcome.ok() && error != nullptr)
            *error = outcome.detail;
        return outcome.ok();
    }
    bool has_active_dsp() const override {
        return ReloadableShellCore::has_active_dsp();
    }
    std::uint64_t contention_blocks() const override {
        return ReloadableShellCore::contention_blocks();
    }
    std::size_t held_retired_logic() const override {
        return ReloadableShellCore::held_retired_logic();
    }
    void reclaim_retired_logic() override {
        ReloadableShellCore::reclaim_retired_logic();
    }

    // The active logic, held so a swap parks it instead of freeing it. The
    // hold is taken under the slot's reader lock, and only the hold: the
    // caller builds after the lock is released, so a swap never waits for an
    // editor. Null when no logic is loaded.
    Processor* hold_active_logic() {
        return with_active_logic([this](Processor& logic) -> Processor* {
            hold_logic(&logic);
            return &logic;
        });
    }
    void release_held_logic(Processor* logic) {
        release_logic(logic);
    }

  private:
    class Factory final : public EditorFactory {
      public:
        explicit Factory(EditorShell& shell) : shell_(shell) {}

        bool supports(EditorRole role) const override {
            auto* logic = shell_.hold_active_logic();
            if (logic == nullptr)
                return false;
            auto* factory = logic->editor_factory();
            const bool supported = factory != nullptr && factory->supports(role);
            shell_.release_held_logic(logic);
            return supported;
        }

        std::unique_ptr<Editor> create(Processor&, EditorContext& context,
                                       EditorRole role) override {
            auto editor = std::make_unique<ForwardingEditor>(shell_, context, role);
            if (!editor->has_logic_editor())
                return nullptr;
            return editor;
        }

      private:
        EditorShell& shell_;
    };

    Factory factory_{*this};
};

ForwardingEditor::ForwardingEditor(EditorShell& shell, EditorContext& context, EditorRole role)
    : shell_(shell), context_(context), role_(role) {
    build();
}

// Build the active logic's editor. The generation is read first, so a swap
// landing during the build is picked up on the next tick.
void ForwardingEditor::build() {
    generation_ = shell_.editor_reload_generation();
    auto* logic = shell_.hold_active_logic();
    if (logic == nullptr)
        return;
    auto* factory = logic->editor_factory();
    std::unique_ptr<Editor> editor;
    if (factory != nullptr && factory->supports(role_))
        editor = factory->create(*logic, context_, role_);
    if (editor == nullptr) {
        shell_.release_held_logic(logic);
        return;
    }
    held_ = logic;
    inner_ = std::move(editor);
    if (size_.width == 0 || size_.height == 0)
        size_ = inner_->size();
}

// Destroy the logic's editor, then release its processor.
void ForwardingEditor::drop() {
    inner_.reset();
    if (held_ != nullptr) {
        shell_.release_held_logic(held_);
        held_ = nullptr;
    }
}

// A hot swap: the old logic's editor goes first, while its processor and image
// are still held; the new one takes the same parent and size.
void ForwardingEditor::rebuild() {
    const auto parent = parent_;
    const auto size = size_;
    if (inner_ != nullptr && parent)
        inner_->detach();
    drop();
    build();
    if (inner_ == nullptr)
        return;
    if (size.width != 0 && size.height != 0 && inner_->set_size(size))
        size_ = inner_->size();
    if (parent && !inner_->attach(*parent))
        parent_.reset();
}

void ForwardingEditor::idle() {
    if (invalidated_)
        return;
    if (shell_.editor_reload_generation() != generation_)
        rebuild();
    if (inner_ != nullptr)
        inner_->idle();
}

} // namespace

std::unique_ptr<Processor> make_reloadable_editor_shell(std::string logic_path) {
    return std::make_unique<EditorShell>(std::move(logic_path));
}

ReloadableEditorShellControl* reloadable_editor_shell_control(Processor& shell) {
    return dynamic_cast<EditorShell*>(&shell);
}

} // namespace pulp::format::reload
