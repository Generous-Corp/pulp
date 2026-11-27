#pragma once

/// @file reloadable_editor_shell.hpp
/// The hot-reloadable plug-in shell for logic libraries that bring their own
/// editor through `Processor::editor_factory()`, with no dependency on any UI
/// library. This is what a `pulp::vellum::ViewProcessor<>` logic library (or
/// any editor-host contract editor) reloads under, in place of
/// `ReloadableShell`, whose `create_view()` forwarding needs Pulp's view layer.
///
/// The shell's own `editor_factory()` hands each `create()` to the ACTIVE
/// logic's factory, through the `Processor` vtable both sides already share; it
/// never casts the logic to a concrete type across the library boundary. Each
/// editor it returns wraps the logic's editor and:
///
/// - holds the logic processor it was built from, so a hot swap parks that
///   processor instead of freeing it (ProcessorHotSwapSlot::hold()); the
///   library image itself is retained, as every reload image is;
/// - on its idle tick, when `editor_reload_generation()` has moved, detaches
///   and destroys the old logic's editor first, releases the hold, then builds
///   the new logic's editor in the same role and attaches it to the same
///   parent at the current size, so nothing calls into the retired logic
///   after that tick;
/// - forwards every other editor-host call to the logic's editor.
///
/// A role the active logic's factory does not offer has no editor. Main thread,
/// like every editor-host call.

#include <pulp/format/editor_host.hpp>
#include <pulp/format/reload/reloadable_shell_core.hpp>

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <utility>

namespace pulp::format::reload {

class ReloadableEditorShell : public ReloadableShellCore {
  public:
    explicit ReloadableEditorShell(std::string logic_path = {})
        : ReloadableShellCore(std::move(logic_path)) {}

    EditorFactory* editor_factory() override {
        return &factory_;
    }

  private:
    class ForwardingEditor final : public Editor {
      public:
        ForwardingEditor(ReloadableEditorShell& shell, EditorContext& context, EditorRole role)
            : shell_(shell), context_(context), role_(role) {
            build();
        }
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
            if (inner_ == nullptr || !inner_->attach(parent))
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
        void idle() override {
            if (shell_.editor_reload_generation() != generation_)
                rebuild();
            if (inner_ != nullptr)
                inner_->idle();
        }
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
        // Build the active logic's editor and hold the logic processor it came
        // from. The generation is read first, so a swap landing during the
        // build is picked up on the next tick.
        void build() {
            generation_ = shell_.editor_reload_generation();
            shell_.with_active_logic([this](Processor& logic) {
                auto* factory = logic.editor_factory();
                if (factory == nullptr || !factory->supports(role_))
                    return false;
                auto editor = factory->create(logic, context_, role_);
                if (editor == nullptr)
                    return false;
                // Hold before the reader lock is released, so a swap cannot
                // free the processor between the build and the hold.
                shell_.hold_logic(&logic);
                held_ = &logic;
                inner_ = std::move(editor);
                if (size_.width == 0 || size_.height == 0)
                    size_ = inner_->size();
                return true;
            });
        }

        // Destroy the logic's editor, then release its processor.
        void drop() {
            inner_.reset();
            if (held_ != nullptr) {
                shell_.release_logic(held_);
                held_ = nullptr;
            }
        }

        // A hot swap: the old logic's editor goes first, while its processor
        // and image are still held; the new one takes the same parent and size.
        void rebuild() {
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

        ReloadableEditorShell& shell_;
        EditorContext& context_;
        EditorRole role_;
        std::unique_ptr<Editor> inner_;
        const Processor* held_ = nullptr;
        std::optional<NativeParent> parent_;
        EditorSize size_{0, 0, 1.0};
        std::uint64_t generation_ = 0;
    };

    class Factory final : public EditorFactory {
      public:
        explicit Factory(ReloadableEditorShell& shell) : shell_(shell) {}

        bool supports(EditorRole role) const override {
            return shell_.with_active_logic([role](Processor& logic) {
                auto* factory = logic.editor_factory();
                return factory != nullptr && factory->supports(role);
            });
        }

        std::unique_ptr<Editor> create(Processor&, EditorContext& context,
                                       EditorRole role) override {
            auto editor = std::make_unique<ForwardingEditor>(shell_, context, role);
            if (!editor->has_logic_editor())
                return nullptr;
            return editor;
        }

      private:
        ReloadableEditorShell& shell_;
    };

    Factory factory_{*this};
};

} // namespace pulp::format::reload
