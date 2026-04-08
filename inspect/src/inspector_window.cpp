// inspector_window.cpp — Tabbed inspector window implementation
#include <pulp/inspect/inspector_window.hpp>

#include <algorithm>
#include <cstdio>
#include <sstream>

namespace pulp::inspect {

// ── Color constants for the inspector chrome ────────────────────────────────
static const Color kBgDark       = Color::rgba8(24, 24, 32);
static const Color kBgPanel      = Color::rgba8(30, 30, 42);
static const Color kBgSection    = Color::rgba8(36, 36, 50);
static const Color kTextPrimary  = Color::rgba8(220, 220, 230);
static const Color kTextMuted    = Color::rgba8(140, 140, 160);
static const Color kBorder       = Color::rgba8(60, 60, 80);

static const Color kLogColor     = Color::rgba8(180, 180, 200);
static const Color kWarnColor    = Color::rgba8(255, 200, 60);
static const Color kErrorColor   = Color::rgba8(255, 80, 80);
static const Color kInfoColor    = Color::rgba8(80, 180, 255);
static const Color kDebugColor   = Color::rgba8(120, 220, 120);

// ── Helpers ─────────────────────────────────────────────────────────────────

static std::string format_float(float v, int decimals = 1) {
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.*f", decimals, static_cast<double>(v));
    return buf;
}

static std::string format_rect(const Rect& r) {
    return format_float(r.x, 0) + ", " + format_float(r.y, 0)
         + "  " + format_float(r.width, 0) + " x " + format_float(r.height, 0);
}

static std::string bool_str(bool v) { return v ? "true" : "false"; }

static std::string direction_str(FlexDirection d) {
    return d == FlexDirection::row ? "row" : "column";
}

/// Create a styled property row: "key: value"
static std::unique_ptr<Label> make_prop_label(const std::string& text, float font_size = 11.0f) {
    auto lbl = std::make_unique<Label>(text);
    lbl->set_font_size(font_size);
    lbl->flex().preferred_height = 18;
    lbl->flex().flex_shrink = 0;
    return lbl;
}

// ── CollapsableSection ─────────────────────────────────────────────────────

CollapsableSection::CollapsableSection(std::string title, bool initially_expanded)
    : title_(std::move(title)), expanded_(initially_expanded)
{
    flex().direction = FlexDirection::column;
    flex().flex_shrink = 0;

    // Header
    auto hdr = std::make_unique<View>();
    hdr->flex().preferred_height = kHeaderHeight;
    hdr->flex().flex_shrink = 0;
    hdr->set_background_color(kBgSection);
    header_ = hdr.get();
    add_child(std::move(hdr));

    // Content container
    auto cnt = std::make_unique<View>();
    cnt->flex().direction = FlexDirection::column;
    cnt->flex().padding = 6;
    cnt->flex().gap = 2;
    cnt->flex().flex_shrink = 0;
    cnt->set_visible(expanded_);
    content_ = cnt.get();
    add_child(std::move(cnt));
}

void CollapsableSection::set_expanded(bool e) {
    expanded_ = e;
    if (content_) content_->set_visible(expanded_);
}

void CollapsableSection::paint(canvas::Canvas& canvas) {
    // Paint header with disclosure triangle and title
    if (!header_) return;
    auto b = header_->bounds();

    // Disclosure triangle
    canvas.set_font("system", 10);
    canvas.set_fill_color(kTextMuted);
    const char* arrow = expanded_ ? "\xe2\x96\xbe" : "\xe2\x96\xb8"; // ▾ or ▸
    canvas.fill_text(arrow, b.x + 8, b.y + b.height * 0.5f + 4);

    // Title
    canvas.set_font("system", 11);
    canvas.set_fill_color(kTextPrimary);
    canvas.fill_text(title_, b.x + 22, b.y + b.height * 0.5f + 4);

    // Bottom border
    canvas.set_fill_color(kBorder);
    canvas.fill_rect(b.x, b.bottom() - 1, b.width, 1);
}

void CollapsableSection::on_mouse_event(const MouseEvent& event) {
    if (!event.is_down) return;
    if (!header_) return;

    // Click on header toggles
    if (event.position.y < kHeaderHeight) {
        set_expanded(!expanded_);
    }
}

// ── ConsoleEntryView ───────────────────────────────────────────────────────

void ConsoleEntryView::set_entry(const ConsoleCapture::Entry& entry) {
    level_ = entry.level;
    message_ = entry.message;

    // Format timestamp as seconds since epoch (simple)
    auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        entry.time.time_since_epoch()).count();
    auto secs = ms / 1000;
    auto frac = ms % 1000;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%lld.%03lld",
                  static_cast<long long>(secs % 100000),
                  static_cast<long long>(frac));
    timestamp_ = buf;
}

void ConsoleEntryView::paint(canvas::Canvas& canvas) {
    auto b = local_bounds();

    // Level badge color
    Color badge_color = kLogColor;
    if (level_ == "warn")       badge_color = kWarnColor;
    else if (level_ == "error") badge_color = kErrorColor;
    else if (level_ == "info")  badge_color = kInfoColor;
    else if (level_ == "debug") badge_color = kDebugColor;

    // Level badge
    canvas.set_font("system", 9);
    canvas.set_fill_color(badge_color.with_alpha(0.2f));
    canvas.fill_rect(b.x + 4, b.y + 3, 36, 16);
    canvas.set_fill_color(badge_color);
    canvas.fill_text(level_, b.x + 8, b.y + 15);

    // Timestamp
    canvas.set_font("system", 9);
    canvas.set_fill_color(kTextMuted);
    canvas.fill_text(timestamp_, b.x + 46, b.y + 15);

    // Message
    canvas.set_font("system", 11);
    canvas.set_fill_color(kTextPrimary);
    canvas.fill_text(message_, b.x + 100, b.y + 15);
}

// ── InspectorWindow ────────────────────────────────────────────────────────

InspectorWindow::~InspectorWindow() {
    // Clear callbacks that capture pointers to our members to prevent
    // dangling references if the root view outlives this window.
    on_view_selected = nullptr;
    if (tree_view_) tree_view_->on_select = nullptr;
    if (tree_view_) tree_view_->on_activate = nullptr;
    inspected_root_ = nullptr;
}

InspectorWindow::InspectorWindow(View& root) : InspectorWindow() {
    set_inspected_root(&root);
}

InspectorWindow::InspectorWindow() {
    flex().direction = FlexDirection::column;
    flex().flex_grow = 1;
    set_background_color(kBgDark);

    auto tab_panel = std::make_unique<TabPanel>();
    tabs_ = tab_panel.get();

    tab_panel->add_tab("Elements", build_elements_tab());
    tab_panel->add_tab("Console", build_console_tab());
    tab_panel->add_tab("Performance", build_performance_tab());
    tab_panel->add_tab("State", build_state_tab());

    tab_panel->set_active_tab(0);
    tab_panel->flex().flex_grow = 1;

    add_child(std::move(tab_panel));
}

// ── Elements tab ────────────────────────────────────────────────────────────

std::unique_ptr<View> InspectorWindow::build_elements_tab() {
    auto root = std::make_unique<View>();
    root->flex().direction = FlexDirection::column;
    root->flex().flex_grow = 1;

    // TreeView (top half — view hierarchy)
    auto tree = std::make_unique<TreeView>();
    tree->flex().preferred_height = 200;
    tree->flex().flex_shrink = 0;
    tree->set_background_color(kBgPanel);
    tree->on_select = [this](TreeNode& node) {
        if (node.user_data) {
            selected_view_ = static_cast<View*>(node.user_data);
            show_properties_for(selected_view_);
            if (on_view_selected) on_view_selected(selected_view_);
        }
    };
    tree_view_ = tree.get();
    root->add_child(std::move(tree));

    // Scrollable property sections below the tree
    auto scroll = std::make_unique<ScrollView>();
    scroll->flex().flex_grow = 1;
    scroll->set_direction(ScrollView::Direction::vertical);

    auto props = std::make_unique<View>();
    props->flex().direction = FlexDirection::column;
    props->flex().flex_shrink = 0;
    elements_props_container_ = props.get();

    // Identity section
    {
        auto section = std::make_unique<CollapsableSection>("Identity");
        auto* content = section->content();
        auto type_lbl = make_prop_label("Type: —");
        prop_type_label_ = type_lbl.get();
        content->add_child(std::move(type_lbl));
        auto id_lbl = make_prop_label("ID: —");
        prop_id_label_ = id_lbl.get();
        content->add_child(std::move(id_lbl));
        identity_section_ = section.get();
        props->add_child(std::move(section));
    }

    // Layout section
    {
        auto section = std::make_unique<CollapsableSection>("Layout");
        auto* content = section->content();

        auto bounds_lbl = make_prop_label("Bounds: —");
        prop_bounds_label_ = bounds_lbl.get();
        content->add_child(std::move(bounds_lbl));

        auto dir_lbl = make_prop_label("Direction: —");
        prop_direction_label_ = dir_lbl.get();
        content->add_child(std::move(dir_lbl));

        auto gap_lbl = make_prop_label("Gap: —");
        prop_gap_label_ = gap_lbl.get();
        content->add_child(std::move(gap_lbl));

        auto pad_lbl = make_prop_label("Padding: —");
        prop_padding_label_ = pad_lbl.get();
        content->add_child(std::move(pad_lbl));

        auto margin_lbl = make_prop_label("Margin: —");
        prop_margin_label_ = margin_lbl.get();
        content->add_child(std::move(margin_lbl));

        auto gs_lbl = make_prop_label("Grow/Shrink: —");
        prop_grow_shrink_label_ = gs_lbl.get();
        content->add_child(std::move(gs_lbl));

        layout_section_ = section.get();
        props->add_child(std::move(section));
    }

    // Visual section
    {
        auto section = std::make_unique<CollapsableSection>("Visual");
        auto* content = section->content();

        auto opacity_lbl = make_prop_label("Opacity: —");
        prop_opacity_label_ = opacity_lbl.get();
        content->add_child(std::move(opacity_lbl));

        auto bg_lbl = make_prop_label("Background: —");
        prop_bgcolor_label_ = bg_lbl.get();
        content->add_child(std::move(bg_lbl));

        auto border_lbl = make_prop_label("Border: —");
        prop_border_label_ = border_lbl.get();
        content->add_child(std::move(border_lbl));

        auto corner_lbl = make_prop_label("Corner radius: —");
        prop_corner_label_ = corner_lbl.get();
        content->add_child(std::move(corner_lbl));

        auto vis_lbl = make_prop_label("Visible: —");
        prop_visible_label_ = vis_lbl.get();
        content->add_child(std::move(vis_lbl));

        auto en_lbl = make_prop_label("Enabled: —");
        prop_enabled_label_ = en_lbl.get();
        content->add_child(std::move(en_lbl));

        visual_section_ = section.get();
        props->add_child(std::move(section));
    }

    // Widget section (hidden by default — shown only for typed widgets)
    {
        auto section = std::make_unique<CollapsableSection>("Widget");
        auto* content = section->content();

        auto val_lbl = make_prop_label("Value: —");
        prop_widget_value_label_ = val_lbl.get();
        content->add_child(std::move(val_lbl));

        section->set_visible(false);
        widget_section_ = section.get();
        props->add_child(std::move(section));
    }

    // Theme Colors section
    {
        auto section = std::make_unique<CollapsableSection>("Theme Colors", false);
        theme_section_ = section.get();
        props->add_child(std::move(section));
    }

    scroll->set_content_size({300, 600});
    scroll->add_child(std::move(props));
    root->add_child(std::move(scroll));

    return root;
}

// ── Console tab ─────────────────────────────────────────────────────────────

std::unique_ptr<View> InspectorWindow::build_console_tab() {
    auto root = std::make_unique<View>();
    root->flex().direction = FlexDirection::column;
    root->flex().flex_grow = 1;
    root->set_background_color(kBgPanel);

    // Header bar
    auto header = std::make_unique<View>();
    header->flex().direction = FlexDirection::row;
    header->flex().preferred_height = 28;
    header->flex().flex_shrink = 0;
    header->flex().padding = 4;
    header->set_background_color(kBgSection);

    auto title = std::make_unique<Label>("Console Output");
    title->set_font_size(11);
    title->flex().flex_grow = 1;
    header->add_child(std::move(title));

    root->add_child(std::move(header));

    // Scrolling log list
    auto scroll = std::make_unique<ScrollView>();
    scroll->flex().flex_grow = 1;
    scroll->set_direction(ScrollView::Direction::vertical);
    console_scroll_ = scroll.get();

    auto list = std::make_unique<View>();
    list->flex().direction = FlexDirection::column;
    list->flex().flex_shrink = 0;
    console_list_ = list.get();
    scroll->add_child(std::move(list));

    root->add_child(std::move(scroll));
    return root;
}

// ── Performance tab ─────────────────────────────────────────────────────────

std::unique_ptr<View> InspectorWindow::build_performance_tab() {
    auto root = std::make_unique<View>();
    root->flex().direction = FlexDirection::column;
    root->flex().flex_grow = 1;
    root->flex().padding = 8;
    root->flex().gap = 4;
    root->set_background_color(kBgPanel);

    // FPS
    auto fps = make_prop_label("FPS: —", 13);
    fps_label_ = fps.get();
    root->add_child(std::move(fps));

    // Frame time
    auto ft = make_prop_label("Frame time: —", 13);
    frame_time_label_ = ft.get();
    root->add_child(std::move(ft));

    // Budget
    auto bgt = make_prop_label("Budget: —", 13);
    budget_label_ = bgt.get();
    root->add_child(std::move(bgt));

    // View count
    auto vc = make_prop_label("View count: —", 13);
    view_count_label_ = vc.get();
    root->add_child(std::move(vc));

    // Separator
    auto sep = std::make_unique<View>();
    sep->flex().preferred_height = 1;
    sep->flex().flex_shrink = 0;
    sep->set_background_color(kBorder);
    root->add_child(std::move(sep));

    // Pass breakdown header
    auto ph = make_prop_label("Per-Pass Breakdown", 12);
    root->add_child(std::move(ph));

    // Pass list container
    auto pl = std::make_unique<View>();
    pl->flex().direction = FlexDirection::column;
    pl->flex().gap = 2;
    pl->flex().flex_shrink = 0;
    pass_list_ = pl.get();
    root->add_child(std::move(pl));

    return root;
}

// ── State tab ───────────────────────────────────────────────────────────────

std::unique_ptr<View> InspectorWindow::build_state_tab() {
    auto root = std::make_unique<View>();
    root->flex().direction = FlexDirection::column;
    root->flex().flex_grow = 1;
    root->set_background_color(kBgPanel);

    // Header
    auto header = std::make_unique<View>();
    header->flex().direction = FlexDirection::row;
    header->flex().preferred_height = 28;
    header->flex().flex_shrink = 0;
    header->flex().padding = 4;
    header->set_background_color(kBgSection);

    auto title = std::make_unique<Label>("Parameters");
    title->set_font_size(11);
    title->flex().flex_grow = 1;
    header->add_child(std::move(title));
    root->add_child(std::move(header));

    // Scrolling param list
    auto scroll = std::make_unique<ScrollView>();
    scroll->flex().flex_grow = 1;
    scroll->set_direction(ScrollView::Direction::vertical);
    state_scroll_ = scroll.get();

    auto list = std::make_unique<View>();
    list->flex().direction = FlexDirection::column;
    list->flex().flex_shrink = 0;
    state_list_ = list.get();
    scroll->add_child(std::move(list));

    root->add_child(std::move(scroll));
    return root;
}

// ── Data binding ────────────────────────────────────────────────────────────

void InspectorWindow::set_inspected_root(View* root) {
    inspected_root_ = root;
    refresh_elements();
}

void InspectorWindow::refresh() {
    if (tabs_) {
        int active = tabs_->active_tab();
        switch (active) {
            case 0: refresh_elements(); break;
            case 1: refresh_console(); break;
            case 2: refresh_performance(); break;
            case 3: refresh_state(); break;
        }
    }
}

// ── Elements refresh ────────────────────────────────────────────────────────

void InspectorWindow::refresh_elements() {
    if (!tree_view_ || !inspected_root_) return;

    // Save the selected view's identity before rebuilding the tree.
    // The old TreeNode pointers become invalid after clear().
    void* saved_selection = nullptr;
    if (tree_view_->selected_node()) {
        saved_selection = tree_view_->selected_node()->user_data;
    }

    // Rebuild tree from inspected root
    auto& root_node = tree_view_->root();
    root_node.children.clear();
    populate_tree_from_view(root_node, inspected_root_);
    root_node.expanded = true;

    // Restore selection: find the new node matching the previously selected view
    if (saved_selection) {
        auto* restored = tree_view_->find_node_by_user_data(saved_selection);
        tree_view_->set_selected_node(restored);
    } else {
        tree_view_->set_selected_node(nullptr);
    }

    // Refresh theme colors section
    if (theme_section_ && inspected_root_) {
        auto* content = theme_section_->content();
        // Clear existing children: remove all by rebuilding
        while (content->child_count() > 0) {
            content->remove_child(content->child_at(0));
        }

        const auto& theme = inspected_root_->theme();
        for (const auto& [name, color] : theme.colors) {
            auto row = std::make_unique<View>();
            row->flex().direction = FlexDirection::row;
            row->flex().preferred_height = 18;
            row->flex().flex_shrink = 0;
            row->flex().gap = 6;

            // Color swatch
            auto swatch = std::make_unique<View>();
            swatch->flex().preferred_width = 14;
            swatch->flex().preferred_height = 14;
            swatch->set_background_color(color);
            swatch->set_border(kBorder, 1, 2);
            row->add_child(std::move(swatch));

            // Token name
            auto lbl = make_prop_label(name, 10);
            lbl->flex().flex_grow = 1;
            row->add_child(std::move(lbl));

            content->add_child(std::move(row));
        }
    }

    // If we still have a selected view, update its properties
    if (selected_view_) {
        show_properties_for(selected_view_);
    }
}

void InspectorWindow::populate_tree_from_view(TreeNode& parent, View* view) {
    if (!view) return;

    auto& node = parent.add_child(ViewInspector::type_name(*view));
    node.user_data = view;
    node.expanded = true;

    if (!view->id().empty()) {
        node.label += " #" + view->id();
    }

    for (size_t i = 0; i < view->child_count(); ++i) {
        populate_tree_from_view(node, view->child_at(i));
    }
}

void InspectorWindow::select_view(View* view) {
    show_properties_for(view);
    if (on_view_selected) on_view_selected(view);
}

void InspectorWindow::show_properties_for(View* view) {
    if (!view) return;

    // Identity
    if (prop_type_label_)
        prop_type_label_->set_text("Type: " + ViewInspector::type_name(*view));
    if (prop_id_label_)
        prop_id_label_->set_text("ID: " + (view->id().empty() ? "(none)" : view->id()));

    // Layout
    if (prop_bounds_label_)
        prop_bounds_label_->set_text("Bounds: " + format_rect(view->bounds()));
    if (prop_direction_label_)
        prop_direction_label_->set_text("Direction: " + direction_str(view->flex().direction));
    if (prop_gap_label_)
        prop_gap_label_->set_text("Gap: " + format_float(view->flex().gap));
    if (prop_padding_label_)
        prop_padding_label_->set_text("Padding: " + format_float(view->flex().padding));
    if (prop_margin_label_)
        prop_margin_label_->set_text("Margin: " + format_float(view->flex().margin));
    if (prop_grow_shrink_label_)
        prop_grow_shrink_label_->set_text("Grow: " + format_float(view->flex().flex_grow)
                                        + " / Shrink: " + format_float(view->flex().flex_shrink));

    // Visual
    if (prop_opacity_label_)
        prop_opacity_label_->set_text("Opacity: " + format_float(view->opacity(), 2));
    if (prop_bgcolor_label_) {
        if (view->has_background_color()) {
            auto c = view->background_color();
            char buf[48];
            std::snprintf(buf, sizeof(buf), "Background: rgba(%d, %d, %d, %.2f)",
                         static_cast<int>(c.r * 255), static_cast<int>(c.g * 255),
                         static_cast<int>(c.b * 255), static_cast<double>(c.a));
            prop_bgcolor_label_->set_text(buf);
        } else {
            prop_bgcolor_label_->set_text("Background: none");
        }
    }
    if (prop_border_label_) {
        if (view->has_border()) {
            prop_border_label_->set_text("Border: " + format_float(view->border_width()) + "px");
        } else {
            prop_border_label_->set_text("Border: none");
        }
    }
    if (prop_corner_label_)
        prop_corner_label_->set_text("Corner radius: " + format_float(view->corner_radius()));
    if (prop_visible_label_)
        prop_visible_label_->set_text("Visible: " + bool_str(view->visible()));
    if (prop_enabled_label_)
        prop_enabled_label_->set_text("Enabled: " + bool_str(view->enabled()));

    // Widget section — show only for typed widgets
    bool has_widget_props = false;
    std::string widget_value;

    if (auto* knob = dynamic_cast<Knob*>(view)) {
        widget_value = "Value: " + format_float(knob->value(), 3);
        if (!knob->label().empty()) widget_value += "  Label: " + knob->label();
        has_widget_props = true;
    } else if (auto* fader = dynamic_cast<Fader*>(view)) {
        widget_value = "Value: " + format_float(fader->value(), 3);
        if (!fader->label().empty()) widget_value += "  Label: " + fader->label();
        has_widget_props = true;
    } else if (auto* toggle = dynamic_cast<Toggle*>(view)) {
        widget_value = "On: " + bool_str(toggle->is_on());
        if (!toggle->label().empty()) widget_value += "  Label: " + toggle->label();
        has_widget_props = true;
    } else if (auto* label = dynamic_cast<Label*>(view)) {
        widget_value = "Text: " + label->text();
        has_widget_props = true;
    } else if (auto* meter = dynamic_cast<Meter*>(view)) {
        widget_value = "RMS: " + format_float(meter->display_rms(), 3)
                     + "  Peak: " + format_float(meter->display_peak(), 3);
        has_widget_props = true;
    }

    if (widget_section_) {
        widget_section_->set_visible(has_widget_props);
        if (has_widget_props && prop_widget_value_label_) {
            prop_widget_value_label_->set_text(widget_value);
        }
    }
}

// ── Console refresh ─────────────────────────────────────────────────────────

void InspectorWindow::refresh_console() {
    if (!console_list_ || !console_capture_) return;

    auto entries = console_capture_->entries();

    // Clear existing
    while (console_list_->child_count() > 0) {
        console_list_->remove_child(console_list_->child_at(0));
    }

    for (const auto& entry : entries) {
        auto row = std::make_unique<ConsoleEntryView>();
        row->set_entry(entry);
        row->flex().preferred_height = 22;
        row->flex().flex_shrink = 0;
        console_list_->add_child(std::move(row));
    }

    if (console_scroll_) {
        console_scroll_->set_content_size({300, static_cast<float>(entries.size()) * 22.0f});
    }
}

// ── Performance refresh ─────────────────────────────────────────────────────

static const char* pass_type_name(render::RenderPassType t) {
    switch (t) {
        case render::RenderPassType::background:   return "background";
        case render::RenderPassType::content:       return "content";
        case render::RenderPassType::effects:       return "effects";
        case render::RenderPassType::overlay:       return "overlay";
        case render::RenderPassType::post_effects:  return "post_effects";
    }
    return "unknown";
}

void InspectorWindow::refresh_performance() {
    if (!rpm_) {
        if (fps_label_) fps_label_->set_text("FPS: (no data)");
        return;
    }

    float total_ms = rpm_->total_time_ms();
    float fps = total_ms > 0 ? 1000.0f / total_ms : 0.0f;

    if (fps_label_)
        fps_label_->set_text("FPS: " + format_float(fps, 1));
    if (frame_time_label_)
        frame_time_label_->set_text("Frame time: " + format_float(total_ms, 2) + " ms");
    if (budget_label_) {
        std::string btext = "Budget: " + format_float(rpm_->budget(), 1) + " ms";
        if (rpm_->over_budget()) btext += "  [OVER BUDGET]";
        budget_label_->set_text(btext);
    }

    // View count
    if (view_count_label_ && inspected_root_) {
        size_t count = ViewInspector::count_views(*inspected_root_);
        view_count_label_->set_text("View count: " + std::to_string(count));
    }

    // Pass breakdown
    if (pass_list_) {
        while (pass_list_->child_count() > 0) {
            pass_list_->remove_child(pass_list_->child_at(0));
        }

        for (const auto& pass : rpm_->passes()) {
            std::string text = std::string(pass_type_name(pass.type))
                             + ": " + format_float(pass.time_ms, 2) + " ms, "
                             + std::to_string(pass.draw_calls) + " draws";
            auto lbl = make_prop_label(text, 11);
            pass_list_->add_child(std::move(lbl));
        }
    }
}

// ── State refresh ───────────────────────────────────────────────────────────

void InspectorWindow::refresh_state() {
    if (!state_list_ || !state_inspector_) return;

    auto params = state_inspector_->all_params();

    while (state_list_->child_count() > 0) {
        state_list_->remove_child(state_list_->child_at(0));
    }

    for (const auto& p : params) {
        auto row = std::make_unique<View>();
        row->flex().direction = FlexDirection::column;
        row->flex().preferred_height = 48;
        row->flex().flex_shrink = 0;
        row->flex().padding = 4;
        row->set_background_color(kBgSection);

        // Name + value
        std::string line1 = p.name + ": " + format_float(p.value, 3);
        if (!p.display_value.empty()) line1 += " (" + p.display_value + ")";
        auto name_lbl = make_prop_label(line1, 11);
        row->add_child(std::move(name_lbl));

        // Range + unit
        std::string line2 = "Range: [" + format_float(p.min, 2) + ", " + format_float(p.max, 2) + "]";
        if (p.step > 0) line2 += " step=" + format_float(p.step, 3);
        if (!p.unit.empty()) line2 += "  Unit: " + p.unit;
        auto range_lbl = make_prop_label(line2, 10);
        row->add_child(std::move(range_lbl));

        // Bottom border
        auto sep = std::make_unique<View>();
        sep->flex().preferred_height = 1;
        sep->flex().flex_shrink = 0;
        sep->set_background_color(kBorder);

        state_list_->add_child(std::move(row));
        state_list_->add_child(std::move(sep));
    }

    if (state_scroll_) {
        float total_height = static_cast<float>(params.size()) * 49.0f;
        state_scroll_->set_content_size({300, total_height});
    }
}

} // namespace pulp::inspect
