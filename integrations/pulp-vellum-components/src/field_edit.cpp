#include <pulp/vellum_components/field_edit.hpp>

#include <cmath>
#include <cstdlib>
#include <iomanip>
#include <sstream>

namespace pulp::vellum_components::overlay {

namespace {

bool is_whole(float v) {
    return std::abs(v - std::round(v)) < 1e-4f;
}

} // namespace

float read_field_value(const View& view, std::string_view path) {
    const auto& f = view.flex();
    if (path == "layout.width")
        return f.preferred_width;
    if (path == "layout.height")
        return f.preferred_height;
    if (path == "layout.padding")
        return f.padding;
    if (path == "layout.margin")
        return f.margin;
    if (path == "style.opacity")
        return view.opacity();
    return 0.0f;
}

void write_field_value(View& view, std::string_view path, float value) {
    auto& f = view.flex();
    if (path == "style.opacity") {
        view.set_opacity(value);
        return;
    }
    if (path == "layout.width")
        f.preferred_width = value;
    else if (path == "layout.height")
        f.preferred_height = value;
    else if (path == "layout.padding")
        f.padding = value;
    else if (path == "layout.margin")
        f.margin = value;
    else
        return;
    // Yoga recomputes from a dirty node, so mark the chain up to the root.
    for (View* v = &view; v; v = v->parent())
        v->invalidate_layout();
}

std::string format_field_value(float value) {
    std::ostringstream out;
    if (is_whole(value))
        out << static_cast<int>(std::round(value));
    else
        out << std::fixed << std::setprecision(2) << value;
    return out.str();
}

void FieldEditBuffer::begin(std::string path, float initial) {
    path_ = std::move(path);
    original_ = initial;
    text_ = format_field_value(initial);
    caret_ = text_.size();
}

void FieldEditBuffer::clear() {
    path_.clear();
    text_.clear();
    caret_ = 0;
    original_ = 0.0f;
}

std::optional<float> FieldEditBuffer::value() const {
    if (text_.empty())
        return std::nullopt;
    char* end = nullptr;
    const double parsed = std::strtod(text_.c_str(), &end);
    if (end == text_.c_str())
        return std::nullopt;
    return static_cast<float>(parsed);
}

bool FieldEditBuffer::insert_digit(char digit) {
    if (digit < '0' || digit > '9')
        return false;
    text_.insert(caret_, 1, digit);
    ++caret_;
    return true;
}

void FieldEditBuffer::backspace() {
    if (caret_ == 0 || text_.empty())
        return;
    text_.erase(caret_ - 1, 1);
    --caret_;
}

void FieldEditBuffer::move_left() {
    if (caret_ > 0)
        --caret_;
}

void FieldEditBuffer::move_right() {
    if (caret_ < text_.size())
        ++caret_;
}

void FieldEditBuffer::move_home() {
    caret_ = 0;
}

void FieldEditBuffer::move_end() {
    caret_ = text_.size();
}

float FieldEditBuffer::nudge(float step) {
    const float current = value().value_or(original_);
    const float next = current + step;
    // A whole result from a whole value shows no fraction, so nudging "8"
    // gives "9" and not "9.00"; a fractional start keeps two decimals.
    if (is_whole(next) && is_whole(current)) {
        text_ = format_field_value(next);
    } else {
        std::ostringstream out;
        out << std::fixed << std::setprecision(2) << next;
        text_ = out.str();
    }
    caret_ = text_.size();
    return next;
}

} // namespace pulp::vellum_components::overlay
