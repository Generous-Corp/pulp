#include <pulp/view/inspector_window.hpp>
#include <pulp/view/input_events.hpp>

namespace pulp::view {

// ── InspectorHighlight ─────────────────────────────────────────────────────

InspectorHighlight::InspectorHighlight() {
    set_hit_testable(false);
    set_visible(true);
    set_id("inspector-highlight");
}

void InspectorHighlight::set_highlight_rect(Rect r) {
    highlight_rect_ = r;
    has_rect_ = true;
}

void InspectorHighlight::clear() {
    has_rect_ = false;
}

void InspectorHighlight::paint(canvas::Canvas& canvas) {
    if (!has_rect_) return;

    // Semi-transparent blue fill
    canvas.set_fill_color(canvas::Color{50, 100, 255, 40});
    canvas.fill_rect(highlight_rect_.x, highlight_rect_.y,
                     highlight_rect_.width, highlight_rect_.height);

    // Blue border
    canvas.set_stroke_color(canvas::Color{50, 100, 255, 180});
    canvas.set_line_width(1.5f);
    canvas.stroke_rect(highlight_rect_.x, highlight_rect_.y,
                       highlight_rect_.width, highlight_rect_.height);
}

// ── InspectorWindow ────────────────────────────────────────────────────────

InspectorWindow::InspectorWindow(View& target_root, WindowManager* mgr,
                                 WindowHost* parent)
    : target_root_(target_root)
    , manager_(mgr)
    , parent_host_(parent)
{
    build_ui();

    // Add highlight overlay to the target root
    highlight_ = std::make_unique<InspectorHighlight>();
    highlight_ptr_ = highlight_.get();
    // Position it to cover the entire target root
    highlight_ptr_->set_bounds(target_root_.local_bounds());
    highlight_ptr_->set_position(View::Position::absolute);
    highlight_ptr_->set_top(0);
    highlight_ptr_->set_left(0);
    highlight_ptr_->set_visible(false);
    target_root_.add_child(std::move(highlight_));
}

InspectorWindow::~InspectorWindow() {
    // Remove highlight from target root
    if (highlight_ptr_) {
        target_root_.remove_child(highlight_ptr_);
        highlight_ptr_ = nullptr;
    }

    // Unregister from window manager
    if (manager_ && window_id_ != 0) {
        manager_->unregister_window(window_id_);
    }
}

void InspectorWindow::build_ui() {
    inspector_root_ = std::make_unique<View>();
    inspector_root_->set_id("inspector-window-root");
    inspector_root_->flex().direction = FlexDirection::column;
    inspector_root_->set_background_color(canvas::Color{30, 30, 36, 255});

    // Header row
    auto header = std::make_unique<View>();
    header->flex().direction = FlexDirection::row;
    header->flex().padding = 8;
    header->flex().preferred_height = 32;
    header->set_background_color(canvas::Color{40, 40, 50, 255});
    header->set_border_bottom(canvas::Color{60, 60, 70, 255}, 1);

    auto title = std::make_unique<Label>("Inspector");
    title->set_font_size(13.0f);
    title->set_font_weight(600);
    title->flex().flex_grow = 1;
    header_label_ = title.get();
    header->add_child(std::move(title));

    inspector_root_->add_child(std::move(header));

    // SplitView: TreeView (left/top) | PropertyList (right/bottom)
    auto split = std::make_unique<SplitView>();
    split->set_orientation(SplitView::Orientation::vertical);
    split->set_split_fraction(0.5f);
    split->set_min_first_size(80);
    split->set_min_second_size(80);
    split->flex().flex_grow = 1;

    // TreeView
    auto tree = std::make_unique<TreeView>();
    tree->set_id("inspector-tree");
    tree->set_row_height(20);
    tree->set_indent(14);
    tree_view_ = tree.get();

    tree_view_->on_select = [this](TreeNode& node) {
        on_tree_select(node);
    };

    split->set_first(std::move(tree));

    // PropertyList
    auto props = std::make_unique<PropertyList>();
    props->set_id("inspector-props");
    props->set_show_categories(true);
    props->set_row_height(22);
    property_list_ = props.get();

    split->set_second(std::move(props));

    inspector_root_->add_child(std::move(split));
}

void InspectorWindow::show() {
    if (window_host_ && window_host_->is_visible()) return;

    refresh();

    if (!window_host_) {
        WindowOptions opts;
        opts.title = "Inspector";
        opts.width = 320;
        opts.height = 500;
        opts.resizable = true;

        WindowType type = WindowType::inspector;
        opts.window_type = &type;

        if (parent_host_)
            opts.parent_native_handle = parent_host_->native_window_handle();

        window_host_ = WindowHost::create(*inspector_root_, opts);
        window_host_->set_close_callback([this]() {
            if (highlight_ptr_)
                highlight_ptr_->set_visible(false);
        });

        if (manager_) {
            window_id_ = manager_->register_window(
                window_host_.get(), inspector_root_.get(),
                WindowType::inspector);
        }
    }

    window_host_->show();

    if (highlight_ptr_)
        highlight_ptr_->set_visible(true);
}

void InspectorWindow::hide() {
    if (window_host_)
        window_host_->hide();

    if (highlight_ptr_)
        highlight_ptr_->set_visible(false);
}

void InspectorWindow::toggle() {
    if (is_visible())
        hide();
    else
        show();
}

bool InspectorWindow::is_visible() const {
    return window_host_ && window_host_->is_visible();
}

void InspectorWindow::refresh() {
    if (!tree_view_) return;

    // Save selected view pointer before rebuilding (old TreeNode pointers become stale)
    void* saved_selection = nullptr;
    if (tree_view_->selected_node()) {
        saved_selection = tree_view_->selected_node()->user_data;
    }

    // Clear and rebuild the tree
    tree_view_->root().children.clear();
    tree_view_->root().label = "Root";
    tree_view_->root().expanded = true;

    ViewInspector::populate_tree(tree_view_->root(), target_root_);

    // Restore selection by finding the matching node in the new tree
    if (saved_selection) {
        tree_view_->set_selected_node(tree_view_->find_node_by_user_data(saved_selection));
    } else {
        tree_view_->set_selected_node(nullptr);
    }

    // Update highlight bounds to match current target root size
    if (highlight_ptr_)
        highlight_ptr_->set_bounds(target_root_.local_bounds());
}

void InspectorWindow::select_view(View* view) {
    selected_view_ = view;

    if (view && property_list_) {
        auto props = ViewInspector::view_properties(*view);
        property_list_->set_properties(std::move(props));
    } else if (property_list_) {
        property_list_->set_properties({});
    }

    update_highlight();
}

void InspectorWindow::install_keyboard_shortcut() {
    target_root_.on_global_key = [this](const KeyEvent& event) -> bool {
        if (!event.is_down) return false;

        // Cmd+I on macOS, Ctrl+I elsewhere
        if (event.isMainModifier() && event.key == KeyCode::i) {
            toggle();
            return true;
        }
        return false;
    };
}

void InspectorWindow::on_tree_select(TreeNode& node) {
    auto* view = static_cast<View*>(node.user_data);
    select_view(view);
}

void InspectorWindow::update_highlight() {
    if (!highlight_ptr_) return;

    if (!selected_view_) {
        highlight_ptr_->clear();
        return;
    }

    Rect abs = ViewInspector::absolute_bounds(*selected_view_);
    highlight_ptr_->set_highlight_rect(abs);
    highlight_ptr_->set_bounds(target_root_.local_bounds());
    highlight_ptr_->set_visible(is_visible());
}

} // namespace pulp::view
