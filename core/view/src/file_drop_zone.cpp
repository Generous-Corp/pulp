#include <pulp/view/file_drop_zone.hpp>

#include <pulp/platform/file_dialog.hpp>

#include <algorithm>

namespace pulp::view {

FileDropZone::FileDropZone() {
    set_access_role(AccessRole::group);
    set_focusable(true);
}

void FileDropZone::set_accepted_extensions(std::vector<std::string> exts) {
    extensions_ = std::move(exts);
}

bool FileDropZone::is_valid_extension(const std::string& path) const {
    if (extensions_.empty()) return true;
    auto dot = path.rfind('.');
    if (dot == std::string::npos) return false;
    auto ext = path.substr(dot);
    std::string lower_ext;
    lower_ext.reserve(ext.size());
    for (char c : ext) lower_ext += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
    for (auto& accepted : extensions_) {
        std::string lower_accepted;
        lower_accepted.reserve(accepted.size());
        for (char c : accepted) lower_accepted += static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
        if (lower_ext == lower_accepted) return true;
    }
    return false;
}

void FileDropZone::drag_enter(const std::vector<std::string>& paths) {
    drag_over_ = true;
    drag_valid_ = true;
    for (auto& p : paths) {
        if (!is_valid_extension(p)) {
            drag_valid_ = false;
            break;
        }
    }
}

void FileDropZone::drag_leave() {
    drag_over_ = false;
    drag_valid_ = false;
}

void FileDropZone::drop(const std::vector<std::string>& paths) {
    drag_over_ = false;
    drag_valid_ = false;
    std::vector<std::string> valid;
    for (auto& p : paths) {
        if (is_valid_extension(p)) valid.push_back(p);
    }
    if (!valid.empty() && on_drop) on_drop(valid);
}

void FileDropZone::on_mouse_event(const MouseEvent& event) {
    if (!browse_on_click_) return;
    const bool press = event.hasExplicitPhase() ? event.isPress() : event.is_down;
    if (!press || event.button != MouseButton::left) return;
    // No dialog backend (iOS / Android / a host that hasn't registered one) →
    // graceful no-op; drag-and-drop remains the path.
    if (!platform::FileDialog::has_backend()) return;

    // One filter built from the accepted extensions (strip leading dots).
    std::vector<platform::FileFilter> filters;
    if (!extensions_.empty()) {
        std::string exts;
        for (const auto& e : extensions_) {
            std::string s = (!e.empty() && e.front() == '.') ? e.substr(1) : e;
            if (!exts.empty()) exts += ';';
            exts += s;
        }
        filters.push_back({"Files", exts});
    }
    auto picked = platform::FileDialog::open_file(dialog_title_, filters, "");
    if (picked && !picked->empty()) drop({*picked});  // same path as a drag drop
}

// ── DropReceiver (drop-dispatch core routes through these) ────────────────────

bool FileDropZone::accept_drag(const DropData& data, Point /*local*/) {
    if (data.type != DropData::Type::files) return false;  // let text/custom bubble
    drag_enter(data.file_paths);
    return true;  // claim the hover (drives highlight)
}

void FileDropZone::leave_drag() { drag_leave(); }

bool FileDropZone::accept_drop(const DropData& data, Point /*local*/) {
    if (data.type != DropData::Type::files) return false;  // let text/custom bubble
    drop(data.file_paths);
    return true;  // the zone owns file drops in its region (consumes the drop)
}

void FileDropZone::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    Color bg, border_c, text_c;
    if (drag_over_ && drag_valid_) {
        auto accent = resolve_color("accent.primary", Color::rgba8(80, 140, 255));
        bg = Color::rgba8(accent.r, accent.g, accent.b, 30);
        border_c = accent;
        text_c = accent;
    } else if (drag_over_) {
        auto err = resolve_color("accent.error", Color::rgba8(255, 80, 80));
        bg = Color::rgba8(err.r, err.g, err.b, 20);
        border_c = err;
        text_c = err;
    } else {
        bg = resolve_color("bg.surface", Color::rgba8(40, 40, 50));
        border_c = resolve_color("control.border", Color::rgba8(80, 80, 90));
        text_c = resolve_color("text.secondary", Color::rgba8(140, 140, 160));
    }

    canvas.set_fill_color(bg);
    canvas.fill_rounded_rect(b.x, b.y, b.width, b.height, 8.0f);
    canvas.set_stroke_color(border_c);
    canvas.set_line_width(2.0f);
    canvas.stroke_rounded_rect(b.x, b.y, b.width, b.height, 8.0f);

    // Icon — simple upload arrow
    if (icon_style_ != IconStyle::none) {
        float icon_size = 32.0f;
        float ix = b.x + b.width / 2 - icon_size / 2;
        float iy = b.y + b.height / 2 - 24;
        float mid_x = ix + icon_size / 2;

        canvas.set_stroke_color(text_c);
        canvas.set_line_width(2.0f);
        canvas.stroke_line(mid_x, iy + 4, mid_x, iy + icon_size - 4);
        canvas.stroke_line(mid_x - 8, iy + 12, mid_x, iy + 4);
        canvas.stroke_line(mid_x + 8, iy + 12, mid_x, iy + 4);
    }

    // Label
    const auto& text = (drag_over_ && drag_valid_) ? hover_label_ : label_;
    float text_y = b.y + b.height / 2 + 16;
    canvas.set_fill_color(text_c);
    canvas.set_font("", 13.0f);
    canvas.set_text_align(canvas::TextAlign::center);
    canvas.fill_text(text, b.x + b.width / 2, text_y);
    canvas.set_text_align(canvas::TextAlign::left);
}

} // namespace pulp::view
