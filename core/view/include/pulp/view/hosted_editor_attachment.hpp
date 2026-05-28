#pragma once

// Hosted-editor → WindowHost attachment (item 4.4).
//
// PR #2844 (native-window embedding) established the canonical contract for
// embedding a platform-native child view inside a Pulp WindowHost via
// WindowHost::attach_native_child_view / set_native_child_view_bounds /
// detach_native_child_view. This header migrates the legacy "void* editor
// handle" hosting path onto that canonical contract.
//
// Before:  a host walked PluginSlot::create_editor_view() -> void* and was
//          responsible for embedding it into whatever native window it owned,
//          with no typed contract for size, resizability, or detach.
//
// After:   a host calls EditorAttachment::create(slot, window) and receives an
//          RAII object that owns the slot's HostedEditor and keeps it attached
//          to the WindowHost for its lifetime. Destroying or releasing the
//          attachment detaches the child view and asks the slot to destroy the
//          editor, in the right order, without the host having to thread the
//          native_handle through any glue code itself.
//
// EditorAttachment is non-copyable, move-constructible. It is null-safe — if
// the slot has no editor or the WindowHost has no factory for native child
// embedding on this platform, create() returns nullptr and the host can fall
// back to a separate top-level window (Phase 6.5 DocumentWindow et al).

// iOS skip: pulp::host (and therefore plugin_slot.hpp) is not on the include
// path on iOS — iOS bundles never host third-party plugins. Use
// `__has_include` so downstream code can include the header unconditionally
// and key off PULP_VIEW_HAS_HOSTED_EDITOR_ATTACHMENT.
#if defined(__has_include) && __has_include(<pulp/host/plugin_slot.hpp>)
#define PULP_VIEW_HAS_HOSTED_EDITOR_ATTACHMENT 1

#include <pulp/host/plugin_slot.hpp>
#include <pulp/view/window_host.hpp>

#include <cstdint>
#include <memory>
#include <utility>

namespace pulp::view {

class EditorAttachment {
public:
    EditorAttachment(const EditorAttachment&) = delete;
    EditorAttachment& operator=(const EditorAttachment&) = delete;

    EditorAttachment(EditorAttachment&& other) noexcept
        : slot_(other.slot_),
          host_(other.host_),
          editor_(std::move(other.editor_)),
          width_(other.width_),
          height_(other.height_),
          attached_(other.attached_) {
        other.slot_ = nullptr;
        other.host_ = nullptr;
        other.attached_ = false;
    }

    EditorAttachment& operator=(EditorAttachment&& other) noexcept {
        if (this != &other) {
            release();
            slot_ = other.slot_;
            host_ = other.host_;
            editor_ = std::move(other.editor_);
            width_ = other.width_;
            height_ = other.height_;
            attached_ = other.attached_;
            other.slot_ = nullptr;
            other.host_ = nullptr;
            other.attached_ = false;
        }
        return *this;
    }

    ~EditorAttachment() { release(); }

    /// Create an attachment by asking the slot for a HostedEditor and embedding
    /// its native_handle into the given WindowHost. Returns nullptr if the slot
    /// has no editor, the editor's native_handle is null, the WindowHost has
    /// no native-window seam on this platform (attach returns false), or the
    /// slot is null.
    static std::unique_ptr<EditorAttachment> create(host::PluginSlot* slot,
                                                    WindowHost* host,
                                                    float x = 0.0f,
                                                    float y = 0.0f) {
        if (!slot || !host) return nullptr;
        auto editor = slot->create_hosted_editor(host->native_window_handle());
        if (!editor || !editor->native_handle) {
            if (editor) slot->destroy_hosted_editor(std::move(editor));
            return nullptr;
        }
        const float w = static_cast<float>(editor->width);
        const float h = static_cast<float>(editor->height);
        const bool ok =
            host->attach_native_child_view(editor->native_handle, x, y, w, h);
        if (!ok) {
            slot->destroy_hosted_editor(std::move(editor));
            return nullptr;
        }
        // The constructor is private; std::make_unique can't see it. Use new.
        return std::unique_ptr<EditorAttachment>(
            new EditorAttachment(slot, host, std::move(editor)));
    }

    /// Underlying typed editor. Never null while the attachment is live.
    const host::PluginSlot::HostedEditor* editor() const { return editor_.get(); }

    /// Whether the editor is currently attached. False after release().
    bool is_attached() const { return attached_; }

    /// Reposition / resize the attached child view. Returns false if the
    /// underlying WindowHost rejects the bounds update (or if the attachment
    /// has already been released).
    bool set_bounds(float x, float y, float width, float height) {
        if (!attached_ || !host_ || !editor_) return false;
        const bool ok = host_->set_native_child_view_bounds(
            editor_->native_handle, x, y, width, height);
        if (ok) {
            width_ = width;
            height_ = height;
        }
        return ok;
    }

    /// Detach the editor from the host and destroy it. Idempotent.
    void release() {
        if (!attached_) {
            // Even if we never managed to attach, we still own the editor and
            // owe the slot a destroy_hosted_editor() call.
            if (editor_ && slot_) {
                slot_->destroy_hosted_editor(std::move(editor_));
            }
            slot_ = nullptr;
            host_ = nullptr;
            return;
        }
        if (host_ && editor_) {
            host_->detach_native_child_view(editor_->native_handle);
        }
        attached_ = false;
        if (slot_ && editor_) {
            slot_->destroy_hosted_editor(std::move(editor_));
        }
        slot_ = nullptr;
        host_ = nullptr;
    }

    float width() const { return width_; }
    float height() const { return height_; }

private:
    EditorAttachment(host::PluginSlot* slot,
                     WindowHost* host,
                     std::unique_ptr<host::PluginSlot::HostedEditor> editor)
        : slot_(slot),
          host_(host),
          editor_(std::move(editor)),
          width_(static_cast<float>(editor_ ? editor_->width : 0)),
          height_(static_cast<float>(editor_ ? editor_->height : 0)),
          attached_(true) {}

    host::PluginSlot* slot_ = nullptr;
    WindowHost* host_ = nullptr;
    std::unique_ptr<host::PluginSlot::HostedEditor> editor_;
    float width_ = 0.0f;
    float height_ = 0.0f;
    bool attached_ = false;
};

} // namespace pulp::view

#else  // !__has_include(<pulp/host/plugin_slot.hpp>)
#define PULP_VIEW_HAS_HOSTED_EDITOR_ATTACHMENT 0
#endif
