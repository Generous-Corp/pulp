#include <pulp/view/code_editor.hpp>
#include <filesystem>
#include <fstream>
#include <algorithm>

namespace pulp::view {

void CodeEditor::configure(const CodeEditorConfig& config) { config_ = config; }
void CodeEditor::set_text(std::string text) { text_ = std::move(text); }
void CodeEditor::set_language(CodeLanguage lang) { config_.language = lang; }
void CodeEditor::set_read_only(bool ro) { config_.read_only = ro; }
void CodeEditor::insert_text(std::string_view text) { text_ += text; }
void CodeEditor::go_to_line(int line) { cursor_line_ = line; }
void CodeEditor::set_markers(const std::vector<std::pair<int, std::string>>&) {}
void CodeEditor::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;
    canvas.set_fill_color(canvas::Color::rgba(30, 30, 35));
    canvas.fill_rect(0, 0, w, h);
    canvas.set_fill_color(canvas::Color::rgba(200, 200, 210));
    canvas.set_font("monospace", config_.font_size);
    canvas.fill_text(text_.empty() ? "// Code editor" : text_.substr(0, 200), 8, 20);
}

SystemTrayIcon::~SystemTrayIcon() { hide(); }
void SystemTrayIcon::set_icon(std::string_view path) { icon_path_ = std::string(path); }
void SystemTrayIcon::set_tooltip(std::string_view text) { tooltip_ = std::string(text); }
void SystemTrayIcon::show() { visible_ = true; }
void SystemTrayIcon::hide() { visible_ = false; }

bool FileBasedDocument::load(std::string_view path) {
    path_ = std::string(path);
    bool ok = load_from_file(path);
    if (ok) dirty_ = false;
    return ok;
}
bool FileBasedDocument::save() {
    if (path_.empty()) return false;
    bool ok = save_to_file(path_);
    if (ok) { dirty_ = false; if (on_dirty_changed) on_dirty_changed(false); }
    return ok;
}
bool FileBasedDocument::save_as(std::string_view path) { path_ = std::string(path); return save(); }
std::string FileBasedDocument::title() const {
    if (path_.empty()) return "Untitled";
    return std::filesystem::path(path_).stem().string();
}

void RecentlyOpenedFilesList::add(std::string_view path) {
    std::string p(path);
    files_.erase(std::remove(files_.begin(), files_.end(), p), files_.end());
    files_.insert(files_.begin(), std::move(p));
    trim();
}
void RecentlyOpenedFilesList::remove(std::string_view path) {
    std::string p(path);
    files_.erase(std::remove(files_.begin(), files_.end(), p), files_.end());
}
bool RecentlyOpenedFilesList::load(std::string_view path) {
    std::ifstream f(std::string(path));
    if (!f) return false;
    files_.clear();
    std::string line;
    while (std::getline(f, line)) if (!line.empty()) files_.push_back(line);
    trim();
    return true;
}
bool RecentlyOpenedFilesList::save(std::string_view path) const {
    std::ofstream f(std::string(path));
    if (!f) return false;
    for (auto& file : files_) f << file << '\n';
    return true;
}
void RecentlyOpenedFilesList::trim() {
    if (static_cast<int>(files_.size()) > max_entries_)
        files_.resize(static_cast<size_t>(max_entries_));
}

}  // namespace pulp::view
