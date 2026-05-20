#include <pulp/view/inspector.hpp>
#include <choc/text/choc_JSON.h>
#include <sstream>

namespace pulp::view {

static choc::value::Value view_to_value(const View& view) {
    auto obj = choc::value::createObject("");

    obj.addMember("type", choc::value::createString(ViewInspector::type_name(view)));

    if (!view.id().empty())
        obj.addMember("id", choc::value::createString(view.id()));

    auto bounds = view.bounds();
    auto bounds_obj = choc::value::createObject("");
    bounds_obj.addMember("x", choc::value::createFloat64(bounds.x));
    bounds_obj.addMember("y", choc::value::createFloat64(bounds.y));
    bounds_obj.addMember("width", choc::value::createFloat64(bounds.width));
    bounds_obj.addMember("height", choc::value::createFloat64(bounds.height));
    obj.addMember("bounds", bounds_obj);

    obj.addMember("visible", choc::value::createBool(view.visible()));

    // Widget-specific properties
    if (auto* knob = dynamic_cast<const Knob*>(&view)) {
        obj.addMember("value", choc::value::createFloat64(knob->value()));
        if (!knob->label().empty())
            obj.addMember("label", choc::value::createString(knob->label()));
    } else if (auto* fader = dynamic_cast<const Fader*>(&view)) {
        obj.addMember("value", choc::value::createFloat64(fader->value()));
        obj.addMember("orientation",
            choc::value::createString(
                fader->orientation() == Fader::Orientation::vertical ? "vertical" : "horizontal"));
    } else if (auto* toggle = dynamic_cast<const Toggle*>(&view)) {
        obj.addMember("on", choc::value::createBool(toggle->is_on()));
    } else if (auto* label = dynamic_cast<const Label*>(&view)) {
        obj.addMember("text", choc::value::createString(label->text()));
    } else if (auto* meter = dynamic_cast<const Meter*>(&view)) {
        obj.addMember("rms", choc::value::createFloat64(meter->display_rms()));
        obj.addMember("peak", choc::value::createFloat64(meter->display_peak()));
    } else if (auto* xy = dynamic_cast<const XYPad*>(&view)) {
        obj.addMember("x", choc::value::createFloat64(xy->x_value()));
        obj.addMember("y", choc::value::createFloat64(xy->y_value()));
    } else if (auto* wf = dynamic_cast<const WaveformView*>(&view)) {
        obj.addMember("samples", choc::value::createInt64(static_cast<int64_t>(wf->sample_count())));
    } else if (auto* sp = dynamic_cast<const SpectrumView*>(&view)) {
        obj.addMember("bins", choc::value::createInt64(static_cast<int64_t>(sp->bin_count())));
    }

    // Children
    if (view.child_count() > 0) {
        auto children = choc::value::createEmptyArray();
        for (size_t i = 0; i < view.child_count(); ++i) {
            children.addArrayElement(view_to_value(*view.child_at(i)));
        }
        obj.addMember("children", children);
    }

    return obj;
}

std::string ViewInspector::to_json(const View& root) {
    auto value = view_to_value(root);
    return choc::json::toString(value, true);
}

View* ViewInspector::find_by_id(View& root, const std::string& id) {
    if (root.id() == id) return &root;
    for (size_t i = 0; i < root.child_count(); ++i) {
        if (auto* found = find_by_id(*root.child_at(i), id))
            return found;
    }
    return nullptr;
}

View* ViewInspector::find_by_anchor(View& root, const std::string& anchor) {
    // An empty anchor never matches — views without a design-import
    // anchor leave anchor_id() empty, and "" should not resolve to the
    // first such view. The inspector's source-jump path relies on this.
    if (anchor.empty()) return nullptr;
    if (root.anchor_id() == anchor) return &root;
    for (size_t i = 0; i < root.child_count(); ++i) {
        if (auto* found = find_by_anchor(*root.child_at(i), anchor))
            return found;
    }
    return nullptr;
}

size_t ViewInspector::count_views(const View& root) {
    size_t count = 1;
    for (size_t i = 0; i < root.child_count(); ++i)
        count += count_views(*root.child_at(i));
    return count;
}

std::string ViewInspector::type_name(const View& view) {
    if (dynamic_cast<const Knob*>(&view)) return "Knob";
    if (dynamic_cast<const Fader*>(&view)) return "Fader";
    if (dynamic_cast<const Toggle*>(&view)) return "Toggle";
    if (dynamic_cast<const Label*>(&view)) return "Label";
    if (dynamic_cast<const Meter*>(&view)) return "Meter";
    if (dynamic_cast<const XYPad*>(&view)) return "XYPad";
    if (dynamic_cast<const WaveformView*>(&view)) return "WaveformView";
    if (dynamic_cast<const SpectrumView*>(&view)) return "SpectrumView";
    return "View";
}

// ── Inspector window helpers ────────────────────────────────────────────────

void ViewInspector::populate_tree(TreeNode& parent, const View& root) {
    std::string label = type_name(root);
    if (!root.id().empty())
        label += " #" + root.id();

    auto& node = parent.add_child(label);
    node.user_data = const_cast<View*>(&root);
    node.expanded = true;

    for (size_t i = 0; i < root.child_count(); ++i)
        populate_tree(node, *root.child_at(i));
}

std::vector<PropertyList::Property> ViewInspector::view_properties(const View& view) {
    std::vector<PropertyList::Property> props;
    using PV = PropertyList::PropertyValue;

    // Identity
    props.push_back({"type", "Type", PV{type_name(view)}, true, "Identity"});
    props.push_back({"id", "ID", PV{view.id().empty() ? std::string("(none)") : view.id()}, true, "Identity"});

    // Layout
    auto b = view.bounds();
    props.push_back({"x", "X", PV{b.x}, true, "Layout"});
    props.push_back({"y", "Y", PV{b.y}, true, "Layout"});
    props.push_back({"width", "Width", PV{b.width}, true, "Layout"});
    props.push_back({"height", "Height", PV{b.height}, true, "Layout"});
    props.push_back({"visible", "Visible", PV{view.visible()}, true, "Layout"});

    // Visual
    props.push_back({"opacity", "Opacity", PV{view.opacity()}, true, "Visual"});
    props.push_back({"has_bg", "Background", PV{view.has_background_color()}, true, "Visual"});
    if (view.has_border())
        props.push_back({"border_w", "Border Width", PV{view.border_width()}, true, "Visual"});
    props.push_back({"corner_r", "Corner Radius", PV{view.corner_radius()}, true, "Visual"});
    props.push_back({"hit_testable", "Hit-Testable", PV{view.hit_testable()}, true, "Visual"});

    // Widget-specific
    if (auto* knob = dynamic_cast<const Knob*>(&view)) {
        props.push_back({"value", "Value", PV{static_cast<float>(knob->value())}, true, "Widget"});
        props.push_back({"label", "Label", PV{knob->label()}, true, "Widget"});
    } else if (auto* fader = dynamic_cast<const Fader*>(&view)) {
        props.push_back({"value", "Value", PV{static_cast<float>(fader->value())}, true, "Widget"});
        props.push_back({"label", "Label", PV{fader->label()}, true, "Widget"});
    } else if (auto* toggle = dynamic_cast<const Toggle*>(&view)) {
        props.push_back({"on", "On", PV{toggle->is_on()}, true, "Widget"});
        props.push_back({"label", "Label", PV{toggle->label()}, true, "Widget"});
    } else if (auto* lbl = dynamic_cast<const Label*>(&view)) {
        props.push_back({"text", "Text", PV{lbl->text()}, true, "Widget"});
        props.push_back({"font_size", "Font Size", PV{lbl->font_size()}, true, "Widget"});
    } else if (auto* meter = dynamic_cast<const Meter*>(&view)) {
        props.push_back({"rms", "RMS", PV{static_cast<float>(meter->display_rms())}, true, "Widget"});
        props.push_back({"peak", "Peak", PV{static_cast<float>(meter->display_peak())}, true, "Widget"});
    }

    return props;
}

Rect ViewInspector::absolute_bounds(const View& view) {
    Rect abs = view.bounds();
    const View* current = view.parent();
    while (current) {
        abs.x += current->bounds().x;
        abs.y += current->bounds().y;
        current = current->parent();
    }
    return abs;
}

} // namespace pulp::view
