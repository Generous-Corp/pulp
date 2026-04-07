#include <pulp/view/file_browser.hpp>
#include <algorithm>

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

void FileTree::refresh() {
    // TreeView integration would go here
}

void FileTree::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();
    canvas.set_fill_color(resolve_color("text.secondary", canvas::Color::rgba(150, 150, 160)));
    canvas.set_font("Inter", 12.0f);
    canvas.fill_text("FileTree: " + root_.string(), 8.0f, 16.0f);
}

// ── ContentSharer ──────────────────────────────────────────────────────

void ContentSharer::share_file(const std::filesystem::path&, void*) {
    // Platform-specific: NSSharingServicePicker on macOS
}

void ContentSharer::share_text(const std::string&, void*) {
    // Platform-specific: NSSharingServicePicker on macOS
}

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
    documents_.erase(documents_.begin() + index);
    if (active_ >= static_cast<int>(documents_.size()))
        active_ = static_cast<int>(documents_.size()) - 1;
    if (on_close) on_close(index);
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
