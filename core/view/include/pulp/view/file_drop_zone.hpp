#pragma once

#include <pulp/view/drag_drop.hpp>
#include <pulp/view/view.hpp>
#include <functional>
#include <vector>
#include <string>

namespace pulp::view {

// ── FileDropZone ────────────────────────────────────────────────────────────
// Drag-and-drop file target with visual feedback. Implements DropReceiver so the
// native drop-dispatch core (drag_drop.cpp) routes file drops here without
// knowing the concrete type.

class FileDropZone : public View, public DropReceiver {
public:
    FileDropZone();

    /// Accepted file extensions (e.g., {".wav", ".aiff", ".mp3"}).
    /// Empty means accept all.
    void set_accepted_extensions(std::vector<std::string> exts);
    const std::vector<std::string>& accepted_extensions() const { return extensions_; }

    /// Label shown when idle.
    void set_label(std::string text) { label_ = std::move(text); }
    const std::string& label() const { return label_; }

    /// Label shown when hovering with valid files.
    void set_hover_label(std::string text) { hover_label_ = std::move(text); }

    /// Icon type to display.
    enum class IconStyle { upload, folder, none };
    void set_icon_style(IconStyle s) { icon_style_ = s; }

    /// Callback when files are dropped.
    std::function<void(const std::vector<std::string>& paths)> on_drop;

    /// Click-to-browse (on by default): a left click opens the platform file
    /// picker (pulp::platform::FileDialog) filtered to `accepted_extensions`,
    /// and feeds the chosen path through the same `on_drop` callback as a drag
    /// drop. Cross-platform — macOS built-in / Linux portal / Windows backend;
    /// a graceful no-op when no dialog backend is installed
    /// (FileDialog::has_backend() == false), so drag-drop still works.
    void set_browse_on_click(bool on) { browse_on_click_ = on; }
    bool browse_on_click() const { return browse_on_click_; }
    /// Title for the click-to-browse dialog (default "Choose a file").
    void set_dialog_title(std::string title) { dialog_title_ = std::move(title); }

    /// Current drag-over state (for external styling).
    bool is_drag_over() const { return drag_over_; }
    bool is_drag_valid() const { return drag_valid_; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_event(const MouseEvent& event) override;
    bool wants_mouse_input() const override { return true; }

    // Called by the platform drag-drop system
    void drag_enter(const std::vector<std::string>& paths);
    void drag_leave();
    void drop(const std::vector<std::string>& paths);

    // DropReceiver — the dispatch core drives these; they delegate to the typed
    // drag_enter/drag_leave/drop above. A FileDropZone consumes file drops in its
    // region (returns true) and ignores text/custom (lets them bubble).
    bool accept_drag(const DropData& data, Point local) override;
    void leave_drag() override;
    bool accept_drop(const DropData& data, Point local) override;

    float intrinsic_height() const override { return 120.0f; }

private:
    std::vector<std::string> extensions_;
    std::string label_ = "Drop files here";
    std::string hover_label_ = "Release to drop";
    IconStyle icon_style_ = IconStyle::upload;
    bool drag_over_ = false;
    bool drag_valid_ = false;
    bool browse_on_click_ = true;
    std::string dialog_title_ = "Choose a file";

    bool is_valid_extension(const std::string& path) const;
};

} // namespace pulp::view
