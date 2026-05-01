#include <pulp/view/file_browser.hpp>
#include <algorithm>
#include <fstream>

// Platform-specific includes must live outside the pulp::view namespace.
// Previously windows.h was included inside the namespace, which on Windows
// ARM64 leaked winbase.h's _Interlocked* intrinsics into pulp::view and
// caused "'pulp::view::_InterlockedCompareExchange': no overloaded
// function could convert all the argument types" build failures. See #92.
#if defined(__APPLE__) || defined(__linux__)
    #include <spawn.h>
    extern "C" { extern char** environ; }
#elif defined(_WIN32)
    #include <windows.h>
    #include <shellapi.h>
#endif

namespace pulp::view {

// ── FileBrowser ────────────────────────────────────────────────────────

FileBrowser::FileBrowser() {
    set_background_color(canvas::Color::rgba(30, 30, 40));
}

void FileBrowser::refresh() {
    entries_.clear();
    if (!std::filesystem::exists(current_)) return;

    try {
        for (auto& entry : std::filesystem::directory_iterator(current_)) {
            if (!show_hidden_ && entry.path().filename().string()[0] == '.') continue;

            Entry e;
            e.name = entry.path().filename().string();
            e.is_directory = entry.is_directory();
            if (!e.is_directory) {
                e.size = entry.file_size();

                // Apply filters
                if (!filters_.empty()) {
                    bool matches = false;
                    auto ext = entry.path().extension().string();
                    for (auto& filter : filters_) {
                        if (filter.size() > 1 && filter[0] == '*') {
                            if (ext == filter.substr(1)) { matches = true; break; }
                        }
                    }
                    if (!matches) continue;
                }
            }
            entries_.push_back(std::move(e));
        }
    } catch (...) {}

    // Sort: directories first, then alphabetical
    std::sort(entries_.begin(), entries_.end(), [](const Entry& a, const Entry& b) {
        if (a.is_directory != b.is_directory) return a.is_directory > b.is_directory;
        return a.name < b.name;
    });
}

void FileBrowser::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    float y = 4.0f;
    float row_h = 24.0f;

    canvas.set_font("Inter", 13.0f);
    canvas.set_text_align(canvas::TextAlign::left);

    for (auto& entry : entries_) {
        auto color = entry.is_directory
            ? resolve_color("accent.primary", canvas::Color::rgba(100, 150, 255))
            : resolve_color("text.primary", canvas::Color::rgba(200, 200, 210));
        canvas.set_fill_color(color);
        canvas.fill_text((entry.is_directory ? "📁 " : "  ") + entry.name, 8.0f, y + 16.0f);
        y += row_h;
        if (y > b.height) break;
    }
}

// ── FileTree ───────────────────────────────────────────────────────────

FileTree::FileTree() {
    set_background_color(canvas::Color::rgba(30, 30, 40));
}

using FileTreeNode = FileTree::FileTreeNode;

static void collect_nodes(const std::filesystem::path& dir, int depth,
                          std::vector<FileTreeNode>& nodes, int max_depth = 4) {
    if (depth > max_depth) return;
    std::error_code ec;
    std::vector<std::filesystem::directory_entry> entries;
    for (auto& entry : std::filesystem::directory_iterator(dir, ec))
        entries.push_back(entry);

    // Sort: directories first, then alphabetical
    std::sort(entries.begin(), entries.end(), [](auto& a, auto& b) {
        if (a.is_directory() != b.is_directory()) return a.is_directory();
        return a.path().filename() < b.path().filename();
    });

    for (auto& entry : entries) {
        auto name = entry.path().filename().string();
        if (name.empty() || name[0] == '.') continue;  // Skip hidden

        FileTreeNode node;
        node.name = name;
        node.path = entry.path();
        node.is_directory = entry.is_directory();
        node.depth = depth;
        nodes.push_back(node);

        if (entry.is_directory() && depth < 1)  // Auto-expand first level
            collect_nodes(entry.path(), depth + 1, nodes, max_depth);
    }
}

void FileTree::refresh() {
    nodes_.clear();
    if (!root_.empty() && std::filesystem::exists(root_))
        collect_nodes(root_, 0, nodes_);
}

void FileTree::paint(canvas::Canvas& canvas) {
    float w = local_bounds().width;
    float row_h = 22.0f;
    float y = 4.0f;

    canvas.set_font("system", 12.0f);

    for (auto& node : nodes_) {
        float indent = 16.0f + node.depth * 16.0f;

        // Icon
        canvas.set_fill_color(node.is_directory
            ? canvas::Color::rgba(200, 180, 80)   // Folder: yellow
            : canvas::Color::rgba(150, 170, 200)); // File: blue-gray
        canvas.fill_text(node.is_directory ? "\xf0\x9f\x93\x81" : "\xf0\x9f\x93\x84",
                        indent - 14.0f, y + 15.0f);

        // Name
        canvas.set_fill_color(canvas::Color::rgba(210, 210, 220));
        canvas.fill_text(node.name, indent, y + 15.0f);

        y += row_h;
        if (y > local_bounds().height) break;
    }
}

// ── ContentSharer ──────────────────────────────────────────────────────
// Uses safe process-launch APIs (no shell interpretation of user input).

#ifdef __APPLE__

void ContentSharer::share_file(const std::filesystem::path& file, void*) {
    // Use posix_spawn with /usr/bin/open — no shell, no injection
    std::string path = file.string();
    const char* argv[] = {"/usr/bin/open", path.c_str(), nullptr};
    pid_t pid;
    posix_spawn(&pid, "/usr/bin/open", nullptr, nullptr,
                const_cast<char**>(argv), environ);
}

void ContentSharer::share_text(const std::string& text, void*) {
    // Write to a unique temp file and open it
    auto tmp = std::filesystem::temp_directory_path() /
               ("pulp_share_" + std::to_string(std::hash<std::string>{}(text)) + ".txt");
    {
        std::ofstream f(tmp);
        f << text;
    }
    share_file(tmp, nullptr);
}

#elif defined(_WIN32)

void ContentSharer::share_file(const std::filesystem::path& file, void*) {
    // ShellExecuteW — safe, no shell interpretation
    std::wstring wpath = file.wstring();
    ShellExecuteW(nullptr, L"open", wpath.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
}

void ContentSharer::share_text(const std::string&, void*) {
    // Windows: no universal text share API; would need UWP DataTransferManager
}

#elif defined(__ANDROID__)
// Android: file sharing is done through JNI/Intent — stub for now
void ContentSharer::share_file(const std::filesystem::path&, void*) {}
void ContentSharer::share_text(const std::string&, void*) {}
#else

void ContentSharer::share_file(const std::filesystem::path& file, void*) {
    // posix_spawnp with xdg-open — uses PATH lookup, no hardcoded path
    std::string path = file.string();
    const char* argv[] = {"xdg-open", path.c_str(), nullptr};
    pid_t pid;
    posix_spawnp(&pid, "xdg-open", nullptr, nullptr,
                const_cast<char**>(argv), environ);
}

void ContentSharer::share_text(const std::string&, void*) {}
#endif

// ── MultiDocumentPanel ─────────────────────────────────────────────────

MultiDocumentPanel::MultiDocumentPanel() {
    set_background_color(canvas::Color::rgba(25, 25, 35));
}

void MultiDocumentPanel::add_document(std::string title, std::unique_ptr<View> content) {
    documents_.push_back({std::move(title), std::move(content)});
    if (active_ < 0) set_active(0);
}

void MultiDocumentPanel::remove_document(int index) {
    if (index < 0 || index >= static_cast<int>(documents_.size())) return;
    const int previous_active = active_;
    const bool removed_active = index == previous_active;
    const bool removed_before_active = index < previous_active;

    documents_.erase(documents_.begin() + index);
    if (documents_.empty()) {
        active_ = -1;
    } else if (removed_before_active) {
        active_ = previous_active - 1;
    } else if (removed_active) {
        active_ = std::min(index, static_cast<int>(documents_.size()) - 1);
    } else if (active_ >= static_cast<int>(documents_.size())) {
        active_ = static_cast<int>(documents_.size()) - 1;
    }

    if (on_close) on_close(index);
    if ((removed_active || removed_before_active || active_ != previous_active) && on_active_changed)
        on_active_changed(active_);
}

void MultiDocumentPanel::set_active(int index) {
    if (index >= 0 && index < static_cast<int>(documents_.size())) {
        active_ = index;
        if (on_active_changed) on_active_changed(index);
    }
}

void MultiDocumentPanel::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Tab bar
    float tab_h = 32.0f;
    float x = 0;
    canvas.set_font("Inter", 12.0f);

    for (int i = 0; i < static_cast<int>(documents_.size()); ++i) {
        float tab_w = canvas.measure_text(documents_[static_cast<size_t>(i)].title) + 24.0f;
        bool active = (i == active_);

        auto bg = active ? resolve_color("bg.surface", canvas::Color::rgba(50, 50, 65))
                          : resolve_color("bg.secondary", canvas::Color::rgba(35, 35, 45));
        canvas.set_fill_color(bg);
        canvas.fill_rect(x, 0, tab_w, tab_h);

        auto text_color = active ? resolve_color("text.primary", canvas::Color::rgba(220, 220, 230))
                                  : resolve_color("text.secondary", canvas::Color::rgba(150, 150, 160));
        canvas.set_fill_color(text_color);
        canvas.set_text_align(canvas::TextAlign::center);
        canvas.fill_text(documents_[static_cast<size_t>(i)].title, x + tab_w / 2.0f, tab_h * 0.65f);

        x += tab_w;
    }
}

} // namespace pulp::view
