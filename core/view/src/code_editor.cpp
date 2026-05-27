#include <pulp/view/code_editor.hpp>
#include <pulp/view/code_editor_tokenizer.hpp>
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
// CodeEditor rendering: native fallback with line numbers and basic highlighting.
// For full Monaco editor with syntax highlighting, minimap, IntelliSense, and
// language modes, use the WebView integration pattern from examples/webview-monaco/.
// That example embeds a bundled Monaco editor via WebViewPanel with the Pulp
// native bridge for bidirectional communication (editor.ready, editor.changed).

void CodeEditor::paint(canvas::Canvas& canvas) {
    float w = bounds().width, h = bounds().height;

    // Background
    auto bg = (config_.theme == "vs-light")
        ? canvas::Color::rgba(255, 255, 255)
        : canvas::Color::rgba(30, 30, 35);
    canvas.set_fill_color(bg);
    canvas.fill_rect(0, 0, w, h);

    float line_h = config_.font_size * 1.5f;
    float gutter_w = config_.line_numbers ? 50.0f : 0.0f;

    // Gutter background
    if (config_.line_numbers) {
        canvas.set_fill_color(canvas::Color::rgba(25, 25, 30));
        canvas.fill_rect(0, 0, gutter_w, h);
    }

    // Split text into lines and render
    canvas.set_font(config_.font_family, config_.font_size);
    canvas.set_text_align(canvas::TextAlign::left);

    auto text_color = (config_.theme == "vs-light")
        ? canvas::Color::rgba(30, 30, 30)
        : canvas::Color::rgba(204, 204, 214);
    auto line_num_color = canvas::Color::rgba(100, 100, 110);
    auto keyword_color = (config_.theme == "vs-light")
        ? canvas::Color::rgba(0, 0, 200)
        : canvas::Color::rgba(86, 156, 214);
    auto type_color = (config_.theme == "vs-light")
        ? canvas::Color::rgba(38, 127, 153)
        : canvas::Color::rgba(78, 201, 176);
    auto number_color = (config_.theme == "vs-light")
        ? canvas::Color::rgba(9, 134, 88)
        : canvas::Color::rgba(181, 206, 168);
    auto preprocessor_color = (config_.theme == "vs-light")
        ? canvas::Color::rgba(175, 0, 219)
        : canvas::Color::rgba(197, 134, 192);
    auto comment_color = canvas::Color::rgba(106, 153, 85);
    auto string_color = canvas::Color::rgba(206, 145, 120);

    auto color_for = [&](TokenClass cls) {
        switch (cls) {
            case TokenClass::keyword:      return keyword_color;
            case TokenClass::type:         return type_color;
            case TokenClass::number:       return number_color;
            case TokenClass::string:       return string_color;
            case TokenClass::comment:      return comment_color;
            case TokenClass::preprocessor: return preprocessor_color;
            case TokenClass::heading:      return keyword_color;
            case TokenClass::link:         return type_color;
            default:                       return text_color;
        }
    };

    float y = line_h;
    int line_num = 1;
    size_t pos = 0;

    while (pos <= text_.size() && y < h) {
        size_t end = text_.find('\n', pos);
        if (end == std::string::npos) end = text_.size();
        std::string line = text_.substr(pos, end - pos);

        // Line number
        if (config_.line_numbers) {
            canvas.set_fill_color(line_num_color);
            canvas.set_text_align(canvas::TextAlign::right);
            canvas.fill_text(std::to_string(line_num), gutter_w - 8.0f, y);
            canvas.set_text_align(canvas::TextAlign::left);
        }

        // Highlight current line
        if (line_num == cursor_line_) {
            canvas.set_fill_color(canvas::Color::rgba(40, 40, 50));
            canvas.fill_rect(gutter_w, y - config_.font_size, w - gutter_w, line_h);
        }

        // Per-token painting via the per-language tokenizer. For
        // `PlainText` we fall back to the historical "first character
        // sniff" so the existing whole-line color contract holds.
        std::vector<Token> tokens;
        if (config_.language != CodeLanguage::PlainText) {
            tokens = tokenize_line(line, config_.language);
        }

        if (config_.language == CodeLanguage::PlainText) {
            bool is_comment = (line.size() >= 2 && line[0] == '/' && line[1] == '/')
                              || (!line.empty() && line[0] == '#');
            if (is_comment) {
                canvas.set_fill_color(comment_color);
            } else if (line.find('"') != std::string::npos
                       || line.find('\'') != std::string::npos) {
                canvas.set_fill_color(string_color);
            } else {
                canvas.set_fill_color(text_color);
            }
            canvas.fill_text(line, gutter_w + 8.0f, y);
        } else {
            // Default paint the entire line as text — token spans paint
            // over it in their typed color. This preserves the "render
            // every visible character" contract the older paint had.
            canvas.set_fill_color(text_color);
            canvas.fill_text(line, gutter_w + 8.0f, y);

            for (const auto& tok : tokens) {
                if (tok.length == 0 || tok.cls == TokenClass::text) continue;
                std::string before = line.substr(0, tok.start);
                std::string span = line.substr(tok.start, tok.length);
                float x_off = canvas.measure_text(before);
                canvas.set_fill_color(color_for(tok.cls));
                canvas.fill_text(span, gutter_w + 8.0f + x_off, y);
            }
        }

        y += line_h;
        line_num++;
        pos = end + 1;
    }
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
    std::string file_path(path);
    std::ifstream f(file_path);
    if (!f) return false;
    files_.clear();
    std::string line;
    while (std::getline(f, line)) if (!line.empty()) files_.push_back(line);
    trim();
    return true;
}
bool RecentlyOpenedFilesList::save(std::string_view path) const {
    std::string file_path(path);
    std::ofstream f(file_path);
    if (!f) return false;
    for (auto& file : files_) f << file << '\n';
    return true;
}
void RecentlyOpenedFilesList::trim() {
    if (static_cast<int>(files_.size()) > max_entries_)
        files_.resize(static_cast<size_t>(max_entries_));
}

}  // namespace pulp::view
