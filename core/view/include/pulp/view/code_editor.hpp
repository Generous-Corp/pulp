#pragma once

// CodeEditorComponent — embeds a Monaco editor via WebView.
// Provides syntax highlighting, line numbers, minimap, and language modes.
// Uses the WebView bridge to communicate between native and editor.

#include <pulp/view/view.hpp>
#include <string>
#include <functional>
#include <optional>

namespace pulp::view {

/// Language mode for syntax highlighting
enum class CodeLanguage {
    PlainText,
    Cpp,
    JavaScript,
    Lua,
    Python,
    JSON,
    XML,
    GLSL,
    Faust,
    JSFX
};

/// Configuration for the code editor
struct CodeEditorConfig {
    CodeLanguage language = CodeLanguage::PlainText;
    bool line_numbers = true;
    bool minimap = false;
    bool word_wrap = true;
    bool read_only = false;
    int tab_size = 4;
    std::string theme = "vs-dark";  // "vs-dark", "vs-light", "hc-black"
    float font_size = 13.0f;
    std::string font_family = "monospace";
};

/// Code editor widget — wraps Monaco editor in a WebView
class CodeEditor : public View {
public:
    CodeEditor() = default;

    /// Set the editor configuration
    void configure(const CodeEditorConfig& config);

    /// Get/set the editor content
    void set_text(std::string text);
    std::string text() const { return text_; }

    /// Set the language mode
    void set_language(CodeLanguage lang);

    /// Set read-only mode
    void set_read_only(bool ro);

    /// Called when the text changes
    std::function<void(const std::string& new_text)> on_change;

    /// Called when the cursor position changes
    std::function<void(int line, int column)> on_cursor_change;

    /// Get cursor position
    int cursor_line() const { return cursor_line_; }
    int cursor_column() const { return cursor_col_; }

    /// Get selected text
    std::string selected_text() const { return selection_; }

    /// Insert text at cursor
    void insert_text(std::string_view text);

    /// Go to a specific line
    void go_to_line(int line);

    /// Set error markers (line → message)
    void set_markers(const std::vector<std::pair<int, std::string>>& markers);

    void paint(canvas::Canvas& canvas) override;

private:
    CodeEditorConfig config_;
    std::string text_;
    std::string selection_;
    int cursor_line_ = 1;
    int cursor_col_ = 1;
    bool initialized_ = false;
};

/// System tray icon (macOS NSStatusItem, Windows Shell_NotifyIcon)
class SystemTrayIcon {
public:
    SystemTrayIcon() = default;
    ~SystemTrayIcon();

    /// Set the tray icon image (path to PNG or system icon name)
    void set_icon(std::string_view icon_path);

    /// Set the tooltip text
    void set_tooltip(std::string_view text);

    /// Show the tray icon
    void show();

    /// Hide the tray icon
    void hide();

    /// Called when the icon is clicked
    std::function<void()> on_click;

    /// Called when the icon is right-clicked (for context menu)
    std::function<void()> on_right_click;

private:
    void* native_handle_ = nullptr;
    std::string icon_path_;
    std::string tooltip_;
    bool visible_ = false;
};

/// FileBasedDocument — document model with save/load/dirty tracking
class FileBasedDocument {
public:
    FileBasedDocument() = default;
    virtual ~FileBasedDocument() = default;

    /// Load from a file path
    bool load(std::string_view path);

    /// Save to the current path
    bool save();

    /// Save to a new path
    bool save_as(std::string_view path);

    /// Whether the document has unsaved changes
    bool is_dirty() const { return dirty_; }
    void set_dirty(bool d = true) { dirty_ = d; }

    /// Current file path
    const std::string& file_path() const { return path_; }

    /// Document title (filename without extension)
    std::string title() const;

    /// Override to implement loading
    virtual bool load_from_file(std::string_view path) = 0;

    /// Override to implement saving
    virtual bool save_to_file(std::string_view path) = 0;

    /// Called when dirty state changes
    std::function<void(bool is_dirty)> on_dirty_changed;

private:
    std::string path_;
    bool dirty_ = false;
};

/// RecentlyOpenedFilesList — persistent MRU list
class RecentlyOpenedFilesList {
public:
    RecentlyOpenedFilesList() = default;

    /// Add a file to the recent list (moves to front if already present)
    void add(std::string_view path);

    /// Remove a file from the list
    void remove(std::string_view path);

    /// Get the list (most recent first)
    const std::vector<std::string>& files() const { return files_; }

    /// Maximum number of entries (default: 10)
    void set_max_entries(int max) { max_entries_ = max; trim(); }

    /// Clear the list
    void clear() { files_.clear(); }

    /// Load from a JSON file
    bool load(std::string_view path);

    /// Save to a JSON file
    bool save(std::string_view path) const;

private:
    std::vector<std::string> files_;
    int max_entries_ = 10;

    void trim();
};

}  // namespace pulp::view
