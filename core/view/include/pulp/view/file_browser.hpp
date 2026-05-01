#pragma once

#include <pulp/view/view.hpp>
#include <pulp/view/tree_view.hpp>
#include <string>
#include <functional>
#include <vector>
#include <filesystem>

namespace pulp::view {

/// File browser component with directory navigation, file list,
/// and preview panel. Similar to system file dialogs but rendered
/// inline as a View.
class FileBrowser : public View {
public:
    FileBrowser();

    /// Set the root directory to browse from.
    void set_root(const std::filesystem::path& root) { root_ = root; current_ = root; refresh(); }
    const std::filesystem::path& root() const { return root_; }

    /// Set the current directory.
    void navigate(const std::filesystem::path& dir) { current_ = dir; refresh(); }
    const std::filesystem::path& current_directory() const { return current_; }

    /// File filter patterns (e.g., {"*.wav", "*.aiff"}).
    void set_filters(std::vector<std::string> patterns) { filters_ = std::move(patterns); refresh(); }
    const std::vector<std::string>& filters() const { return filters_; }

    /// Whether to show hidden files.
    void set_show_hidden(bool v) { show_hidden_ = v; refresh(); }
    bool show_hidden() const { return show_hidden_; }

    /// The selected file (empty if no selection).
    const std::filesystem::path& selected_file() const { return selected_; }

    /// Callback when a file is selected (single click).
    std::function<void(const std::filesystem::path&)> on_select;

    /// Callback when a file is activated (double click or enter).
    std::function<void(const std::filesystem::path&)> on_activate;

    void paint(canvas::Canvas& canvas) override;

private:
    void refresh();

    std::filesystem::path root_;
    std::filesystem::path current_;
    std::filesystem::path selected_;
    std::vector<std::string> filters_;
    bool show_hidden_ = false;

    struct Entry {
        std::string name;
        bool is_directory = false;
        uintmax_t size = 0;
    };
    std::vector<Entry> entries_;
};

/// File tree component — tree-structured directory view.
/// Uses TreeView internally for hierarchical display.
class FileTree : public View {
public:
    FileTree();

    void set_root(const std::filesystem::path& root) { root_ = root; refresh(); }
    const std::filesystem::path& root() const { return root_; }

    /// Callback when a file node is selected.
    std::function<void(const std::filesystem::path&)> on_select;

    void paint(canvas::Canvas& canvas) override;

    struct FileTreeNode {
        std::string name;
        std::filesystem::path path;
        bool is_directory = false;
        bool expanded = false;
        int depth = 0;
    };

private:
    void refresh();
    std::filesystem::path root_;
    std::vector<FileTreeNode> nodes_;
};

/// Content sharer — platform-native share sheet for files/text.
/// On macOS: NSSharingServicePicker. On other platforms: stub.
class ContentSharer {
public:
    /// Share a file via the system share sheet.
    static void share_file(const std::filesystem::path& file, void* parent_window = nullptr);

    /// Share text via the system share sheet.
    static void share_text(const std::string& text, void* parent_window = nullptr);
};

/// Multi-document panel — tabbed container for multiple document views.
/// Each tab hosts an independent View with its own content.
class MultiDocumentPanel : public View {
public:
    MultiDocumentPanel();

    /// Add a document tab.
    void add_document(std::string title, std::unique_ptr<View> content);

    /// Remove a document tab by index.
    void remove_document(int index);

    /// Get the active document index (-1 if none).
    int active_index() const { return active_; }

    /// Set the active document.
    void set_active(int index);

    /// Number of open documents.
    int document_count() const { return static_cast<int>(documents_.size()); }

    /// Callback when a document is closed.
    std::function<void(int index)> on_close;

    /// Callback when the active document changes.
    std::function<void(int index)> on_active_changed;

    void paint(canvas::Canvas& canvas) override;

private:
    struct Document {
        std::string title;
        std::unique_ptr<View> content;
    };
    std::vector<Document> documents_;
    int active_ = -1;
};

} // namespace pulp::view
