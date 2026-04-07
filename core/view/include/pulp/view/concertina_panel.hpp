#pragma once

// ConcertinaPanel — accordion-style collapsible panel stack.
// Each section has a header that can be clicked to expand/collapse.

#include <pulp/view/view.hpp>
#include <string>
#include <vector>
#include <memory>
#include <functional>

namespace pulp::view {

class ConcertinaPanel : public View {
public:
    ConcertinaPanel() = default;

    /// Add a section with a title and content view.
    /// The content view is owned by the ConcertinaPanel.
    void add_section(std::string title, std::unique_ptr<View> content,
                     bool initially_expanded = false);

    /// Expand a section by index.
    void expand(int index);

    /// Collapse a section by index.
    void collapse(int index);

    /// Toggle a section.
    void toggle(int index);

    /// Whether a section is expanded.
    bool is_expanded(int index) const;

    /// Number of sections.
    int section_count() const { return static_cast<int>(sections_.size()); }

    /// Header height for each section.
    void set_header_height(float h) { header_height_ = h; }
    float header_height() const { return header_height_; }

    /// Whether only one section can be open at a time (default: false).
    void set_exclusive(bool e) { exclusive_ = e; }
    bool is_exclusive() const { return exclusive_; }

    void paint(canvas::Canvas& canvas) override;
    void on_mouse_down(Point pos) override;

    /// Layout the sections vertically.
    void layout() override;

private:
    struct Section {
        std::string title;
        std::unique_ptr<View> content;
        bool expanded = false;
    };

    std::vector<Section> sections_;
    float header_height_ = 30.0f;
    bool exclusive_ = false;
};

}  // namespace pulp::view
