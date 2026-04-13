#include <pulp/format/view_bridge.hpp>
#include <pulp/format/editor_ui.hpp>
#include <pulp/view/scripted_ui.hpp>
#include <pulp/view/view.hpp>

namespace pulp::format {

ViewBridge::ViewBridge(Processor& processor, state::StateStore& store)
    : ViewBridge(processor, store, Options{}) {}

ViewBridge::ViewBridge(Processor& processor, state::StateStore& store, Options options)
    : processor_(processor),
      store_(store),
      options_(options),
      size_hints_(processor.view_size()) {
    width_ = size_hints_.preferred_width;
    height_ = size_hints_.preferred_height;
}

ViewBridge::~ViewBridge() {
    close();
}

bool ViewBridge::open(std::string* error) {
    if (view_) return true;
    last_error_.clear();

    // First chance: let the processor supply a fully custom view.
    auto custom = processor_.create_view();
    if (custom) {
        view_ = std::move(custom);
    } else {
        // Fall back to the scripted-UI or AutoUi default.
        auto instance = build_editor_ui(store_, options_.enable_hot_reload, &last_error_);
        if (!instance.root) {
            if (error) *error = last_error_.empty() ? "ViewBridge: failed to build editor UI" : last_error_;
            return false;
        }
        view_ = std::move(instance.root);
        scripted_ui_ = std::move(instance.scripted_ui);
        uses_script_ui_ = instance.uses_script_ui;
    }

    size_hints_ = processor_.view_size();
    width_ = size_hints_.preferred_width;
    height_ = size_hints_.preferred_height;
    attached_ = false;
    return true;
}

void ViewBridge::notify_attached() {
    if (!view_ || attached_) return;
    attached_ = true;
    processor_.on_view_opened(*view_);
}

void ViewBridge::close() {
    if (!view_) return;
    if (attached_) {
        processor_.on_view_closed(*view_);
        attached_ = false;
    }
    scripted_ui_.reset();
    view_.reset();
    uses_script_ui_ = false;
    secondaries_.clear();
}

void ViewBridge::resize(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    if (view_ && attached_) {
        processor_.on_view_resized(*view_, width, height);
    }
}

view::View* ViewBridge::attach_secondary_view(std::unique_ptr<view::View> v, ViewRole role) {
    if (!v) return nullptr;
    auto* raw = v.get();
    secondaries_.push_back({std::move(v), role});
    return raw;
}

bool ViewBridge::detach_secondary_view(view::View* target) {
    for (auto it = secondaries_.begin(); it != secondaries_.end(); ++it) {
        if (it->view.get() == target) {
            secondaries_.erase(it);
            return true;
        }
    }
    return false;
}

size_t ViewBridge::view_count() const {
    return (view_ ? 1u : 0u) + secondaries_.size();
}

view::View* ViewBridge::view_at(size_t index) {
    if (view_) {
        if (index == 0) return view_.get();
        --index;
    }
    if (index < secondaries_.size()) return secondaries_[index].view.get();
    return nullptr;
}

ViewRole ViewBridge::role_at(size_t index) const {
    if (view_) {
        if (index == 0) return options_.role;
        --index;
    }
    if (index < secondaries_.size()) return secondaries_[index].role;
    return ViewRole::Editor;
}

} // namespace pulp::format
