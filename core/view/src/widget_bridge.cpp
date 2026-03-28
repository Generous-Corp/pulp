#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <cmath>

namespace pulp::view {

static const char* kJSPreamble = R"(
var __callbacks__ = {};
function __dispatch__(id, eventName, value) {
    var key = id + ':' + eventName;
    if (__callbacks__[key]) __callbacks__[key](value);
}
function on(id, eventName, fn) {
    __callbacks__[id + ':' + eventName] = fn;
}
)";

WidgetBridge::WidgetBridge(ScriptEngine& engine, View& root, state::StateStore& store)
    : engine_(engine), root_(root), store_(store) {
    register_api();
    engine_.evaluate(kJSPreamble);
}

void WidgetBridge::load_script(const std::string& code) {
    engine_.evaluate(code);
}

View* WidgetBridge::widget(const std::string& id) {
    auto it = widgets_.find(id);
    return it != widgets_.end() ? it->second : nullptr;
}

void WidgetBridge::sync_from_store() {
    for (auto& [id, view] : widgets_) {
        if (auto* knob = dynamic_cast<Knob*>(view)) {
            // Try to find a parameter matching this widget's id
            // Convention: widget id matches parameter name
            for (size_t i = 0; i < store_.param_count(); ++i) {
                auto* info = &store_.all_params()[i];
                if (info && info->name == id) {
                    knob->set_value(store_.get_normalized(info->id));
                    break;
                }
            }
        } else if (auto* fader = dynamic_cast<Fader*>(view)) {
            for (size_t i = 0; i < store_.param_count(); ++i) {
                auto* info = &store_.all_params()[i];
                if (info && info->name == id) {
                    fader->set_value(store_.get_normalized(info->id));
                    break;
                }
            }
        } else if (auto* toggle = dynamic_cast<Toggle*>(view)) {
            for (size_t i = 0; i < store_.param_count(); ++i) {
                auto* info = &store_.all_params()[i];
                if (info && info->name == id) {
                    toggle->set_on(store_.get_normalized(info->id) > 0.5f);
                    break;
                }
            }
        }
    }
}

View* WidgetBridge::resolve_parent(const std::string& parent_id) {
    if (parent_id.empty()) return &root_;
    auto it = widgets_.find(parent_id);
    return it != widgets_.end() ? it->second : &root_;
}

void WidgetBridge::wire_callbacks(const std::string& id, View* w) {
    if (auto* k = dynamic_cast<Knob*>(w)) {
        k->on_change = [this, id](float v) {
            engine_.evaluate("__dispatch__('" + id + "', 'change', " + std::to_string(v) + ")");
        };
    } else if (auto* f = dynamic_cast<Fader*>(w)) {
        f->on_change = [this, id](float v) {
            engine_.evaluate("__dispatch__('" + id + "', 'change', " + std::to_string(v) + ")");
        };
    } else if (auto* t = dynamic_cast<Toggle*>(w)) {
        t->on_toggle = [this, id](bool v) {
            engine_.evaluate("__dispatch__('" + id + "', 'toggle', " + std::string(v?"1":"0") + ")");
        };
    }
}

void WidgetBridge::clear() {
    std::vector<std::string> ids;
    ids.reserve(widgets_.size());
    for (auto& [id, _] : widgets_) ids.push_back(id);
    for (auto& id : ids) {
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            View* w = it->second;
            if (auto* p = w->parent()) p->remove_child(w);
        }
    }
    widgets_.clear();
}

void WidgetBridge::snapshot_values(std::unordered_map<std::string, float>& out) const {
    for (auto& [id, view] : widgets_) {
        if (auto* k = dynamic_cast<Knob*>(view)) out[id] = k->value();
        else if (auto* f = dynamic_cast<Fader*>(view)) out[id] = f->value();
        else if (auto* t = dynamic_cast<Toggle*>(view)) out[id] = t->is_on() ? 1.0f : 0.0f;
    }
}

void WidgetBridge::restore_values(const std::unordered_map<std::string, float>& snapshot) {
    for (auto& [id, val] : snapshot) {
        auto it = widgets_.find(id);
        if (it == widgets_.end()) continue;
        if (auto* k = dynamic_cast<Knob*>(it->second)) k->set_value(val);
        else if (auto* f = dynamic_cast<Fader*>(it->second)) f->set_value(val);
        else if (auto* t = dynamic_cast<Toggle*>(it->second)) t->set_on(val > 0.5f);
    }
}

void WidgetBridge::register_api() {
    // Helper: detect if arg[1] is a parent ID (string) or x position (number)
    // Old API: createKnob(id, x, y, w, h) — 5 args, arg[1] is number
    // New API: createKnob(id, parentId) — 2 args, arg[1] is string
    auto isNewApi = [](choc::javascript::ArgumentList& args) -> bool {
        if (args.numArgs <= 2) return true; // Only id + optional parent
        // If 3+ args and arg[2] looks numeric, it's old API (id, x, y, ...)
        auto test = args.get<std::string>(1, "");
        // If the string is a registered widget ID or empty, it's new API
        // If it parses as "0" or similar number, it's old API
        if (test.empty()) return true;
        if (test[0] >= '0' && test[0] <= '9') return false;
        if (test[0] == '-') return false;
        return true; // Starts with a letter = parent ID
    };

    // createKnob(id, parentId) OR createKnob(id, x, y, w, h)
    engine_.register_function("createKnob", [this, isNewApi](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto knob = std::make_unique<Knob>();
        knob->set_id(id);
        knob->set_label(id);

        if (isNewApi(args)) {
            auto pid = args.get<std::string>(1, "");
            auto* ptr = knob.get();
            widgets_[id] = ptr;
            wire_callbacks(id, ptr);
            resolve_parent(pid)->add_child(std::move(knob));
        } else {
            knob->set_bounds({(float)args.get<double>(1,0), (float)args.get<double>(2,0),
                             (float)args.get<double>(3,48), (float)args.get<double>(4,48)});
            widgets_[id] = knob.get();
            root_.add_child(std::move(knob));
        }
        return choc::value::createString(id);
    });

    // createFader(id, orientation, parentId) OR createFader(id, x, y, w, h, orientation)
    engine_.register_function("createFader", [this, isNewApi](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto fader = std::make_unique<Fader>();
        fader->set_id(id);

        if (isNewApi(args)) {
            auto orient = args.get<std::string>(1, "vertical");
            auto pid = args.get<std::string>(2, "");
            if (orient == "horizontal") fader->set_orientation(Fader::Orientation::horizontal);
            auto* ptr = fader.get();
            widgets_[id] = ptr;
            wire_callbacks(id, ptr);
            resolve_parent(pid)->add_child(std::move(fader));
        } else {
            fader->set_bounds({(float)args.get<double>(1,0), (float)args.get<double>(2,0),
                              (float)args.get<double>(3,24), (float)args.get<double>(4,200)});
            auto orient = args.get<std::string>(5, "vertical");
            if (orient == "horizontal") fader->set_orientation(Fader::Orientation::horizontal);
            fader->set_label(id);
            widgets_[id] = fader.get();
            root_.add_child(std::move(fader));
        }
        return choc::value::createString(id);
    });

    // createToggle(id, parentId) OR createToggle(id, x, y, w, h)
    engine_.register_function("createToggle", [this, isNewApi](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto toggle = std::make_unique<Toggle>();
        toggle->set_id(id);

        if (isNewApi(args)) {
            auto pid = args.get<std::string>(1, "");
            auto* ptr = toggle.get();
            widgets_[id] = ptr;
            wire_callbacks(id, ptr);
            resolve_parent(pid)->add_child(std::move(toggle));
        } else {
            toggle->set_bounds({(float)args.get<double>(1,0), (float)args.get<double>(2,0),
                               (float)args.get<double>(3,50), (float)args.get<double>(4,30)});
            toggle->set_label(id);
            widgets_[id] = toggle.get();
            root_.add_child(std::move(toggle));
        }
        return choc::value::createString(id);
    });

    // createIcon(id, type, parentId) — type: "image_upload", "send", "search", "close"
    engine_.register_function("createIcon", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto type_str = args.get<std::string>(1, "image_upload");
        auto pid = args.get<std::string>(2, "");
        auto icon = std::make_unique<Icon>();
        icon->set_id(id);
        if (type_str == "send") icon->set_type(Icon::Type::send);
        else if (type_str == "search") icon->set_type(Icon::Type::search);
        else if (type_str == "close") icon->set_type(Icon::Type::close);
        else icon->set_type(Icon::Type::image_upload);
        widgets_[id] = icon.get();
        resolve_parent(pid)->add_child(std::move(icon));
        return choc::value::createString(id);
    });

    // createCheckbox(id, parentId)
    engine_.register_function("createCheckbox", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto cb = std::make_unique<Checkbox>(); cb->set_id(id);
        auto* ptr = cb.get(); widgets_[id] = ptr;
        cb->on_change = [this, id](bool v) {
            engine_.evaluate("__dispatch__('" + id + "', 'change', " + std::string(v?"1":"0") + ")");
        };
        resolve_parent(pid)->add_child(std::move(cb));
        return choc::value::createString(id);
    });

    // createToggleButton(id, parentId)
    engine_.register_function("createToggleButton", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto tb = std::make_unique<ToggleButton>(); tb->set_id(id);
        auto* ptr = tb.get(); widgets_[id] = ptr;
        tb->on_toggle = [this, id](bool v) {
            engine_.evaluate("__dispatch__('" + id + "', 'toggle', " + std::string(v?"1":"0") + ")");
        };
        resolve_parent(pid)->add_child(std::move(tb));
        return choc::value::createString(id);
    });

    // createLabel(id, text, parentId) OR createLabel(id, text, x, y, w, h)
    engine_.register_function("createLabel", [this, isNewApi](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto text = args.get<std::string>(1, "");

        // For Label: old API is (id, text, x, y, w, h) — arg[2] is number
        // New API is (id, text, parentId) — arg[2] is string or absent
        bool old = false;
        if (args.numArgs >= 4) {
            auto test = args.get<std::string>(2, "");
            if (!test.empty() && (test[0] >= '0' && test[0] <= '9')) old = true;
        }

        auto label = std::make_unique<Label>(text);
        label->set_id(id);

        if (old) {
            label->set_bounds({(float)args.get<double>(2,0), (float)args.get<double>(3,0),
                              (float)args.get<double>(4,100), (float)args.get<double>(5,20)});
            widgets_[id] = label.get();
            root_.add_child(std::move(label));
        } else {
            auto pid = args.get<std::string>(2, "");
            widgets_[id] = label.get();
            resolve_parent(pid)->add_child(std::move(label));
        }
        return choc::value::createString(id);
    });

    // Keep old-API registration paths below for backward compat
    // (The above replacements handle both APIs)

    // ── OLD createFader removed (replaced above) ──
    // Skip re-registering — the lambda above handles both APIs

    // Continue with other registrations...
    // (Old createFader/Toggle/Label registrations removed — replaced with
    // dual-API versions above that detect parent ID vs absolute positioning)

    // setValue(id, value) -> set widget normalized value
    engine_.register_function("setValue", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto value = args.get<double>(1, 0);

        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::Value();

        if (auto* knob = dynamic_cast<Knob*>(it->second))
            knob->set_value(static_cast<float>(value));
        else if (auto* fader = dynamic_cast<Fader*>(it->second))
            fader->set_value(static_cast<float>(value));
        else if (auto* toggle = dynamic_cast<Toggle*>(it->second))
            toggle->set_on(value > 0.5);
        else if (auto* cb = dynamic_cast<Checkbox*>(it->second))
            cb->set_checked(value > 0.5);
        else if (auto* tb = dynamic_cast<ToggleButton*>(it->second))
            tb->set_on(value > 0.5);

        return choc::value::Value();
    });

    // getValue(id) -> get widget normalized value
    engine_.register_function("getValue", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");

        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::createFloat64(0);

        if (auto* knob = dynamic_cast<Knob*>(it->second))
            return choc::value::createFloat64(knob->value());
        if (auto* fader = dynamic_cast<Fader*>(it->second))
            return choc::value::createFloat64(fader->value());
        if (auto* toggle = dynamic_cast<Toggle*>(it->second))
            return choc::value::createFloat64(toggle->is_on() ? 1.0 : 0.0);

        return choc::value::createFloat64(0);
    });

    // getParam(name) -> get parameter value from store (normalized)
    engine_.register_function("getParam", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");

        for (size_t i = 0; i < store_.param_count(); ++i) {
            auto* info = &store_.all_params()[i];
            if (info && info->name == name) {
                return choc::value::createFloat64(store_.get_normalized(info->id));
            }
        }
        return choc::value::createFloat64(0);
    });

    // setParam(name, normalized_value) -> set parameter in store
    engine_.register_function("setParam", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto value = args.get<double>(1, 0);

        for (size_t i = 0; i < store_.param_count(); ++i) {
            auto* info = &store_.all_params()[i];
            if (info && info->name == name) {
                store_.set_normalized(info->id, static_cast<float>(value));
                break;
            }
        }
        return choc::value::Value();
    });

    // ═══════════════════════════════════════════════════════════════════
    // Animation bridge
    // ═══════════════════════════════════════════════════════════════════

    // animate(id, property, targetValue, durationMs, easingName)
    engine_.register_function("animate", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto prop = args.get<std::string>(1, "value");
        auto target = static_cast<float>(args.get<double>(2, 0));
        auto dur_ms = static_cast<float>(args.get<double>(3, 150));
        auto ease_name = args.get<std::string>(4, "ease_out_cubic");

        auto it = widgets_.find(id);
        if (it == widgets_.end()) return choc::value::Value();

        float dur = dur_ms / 1000.0f;
        auto ease = easing_by_name(ease_name);

        if (prop == "value") {
            if (auto* k = dynamic_cast<Knob*>(it->second))
                k->set_value(target); // immediate for now — knob value isn't animated
            else if (auto* f = dynamic_cast<Fader*>(it->second))
                f->set_value(target);
            else if (auto* t = dynamic_cast<Toggle*>(it->second))
                t->set_on(target > 0.5f);
        }
        (void)dur; (void)ease; // duration/easing used by widget-local animations
        return choc::value::Value();
    });

    // setMotionToken(tokenName, value)
    engine_.register_function("setMotionToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto value = static_cast<float>(args.get<double>(1, 0));
        if (name.empty()) return choc::value::Value();
        auto theme = root_.theme();
        theme.dimensions[name] = value;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // getMotionToken(tokenName) -> value
    engine_.register_function("getMotionToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto d = root_.theme().dimension(name);
        return choc::value::createFloat64(d.value_or(0.0f));
    });

    // setVisible(id, bool)
    engine_.register_function("setVisible", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto vis = args.get<double>(1, 1) > 0.5;
        auto it = widgets_.find(id);
        if (it != widgets_.end()) it->second->set_visible(vis);
        return choc::value::Value();
    });

    // registerHover(id) — enables "mouseenter"/"mouseleave" JS callbacks (CSS :hover)
    engine_.register_function("registerHover", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            it->second->on_hover_enter = [this, id]() {
                engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
            };
            it->second->on_hover_leave = [this, id]() {
                engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
            };
        }
        return choc::value::Value();
    });

    // registerClick(id) — enables "click" event dispatch for any widget
    engine_.register_function("registerClick", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            it->second->on_click = [this, id]() {
                engine_.evaluate("__dispatch__('" + id + "', 'click', 0)");
            };
        }
        return choc::value::Value();
    });

    // enableInspectClick() — sets up Cmd+click detection on all registered widgets
    engine_.register_function("enableInspectClick", [this](choc::javascript::ArgumentList) {
        root_.on_global_click = [this](const std::string& id, uint16_t mods) {
            // Check for Cmd modifier (kModCmd = 0x10, kModMeta = 0x08)
            bool cmd = (mods & (0x10 | 0x08)) != 0;
            if (cmd) {
                engine_.evaluate("__dispatch__('__inspect__', 'click', '" + id + "')");
            }
        };
        return choc::value::Value();
    });

    // removeWidget(id)
    engine_.register_function("removeWidget", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            View* w = it->second;
            View* parent = w->parent();
            if (parent) parent->remove_child(w);
            widgets_.erase(it);
        }
        return choc::value::Value();
    });

    // ═══════════════════════════════════════════════════════════════
    // Extended API: containers, layout, all widgets, themes, canvas
    // ═══════════════════════════════════════════════════════════════

    engine_.register_function("createRow", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<View>(); v->set_id(id);
        v->flex().direction = FlexDirection::row;
        widgets_[id] = v.get();
        resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    engine_.register_function("createCol", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<View>(); v->set_id(id);
        v->flex().direction = FlexDirection::column;
        widgets_[id] = v.get();
        resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    engine_.register_function("createPanel", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<Panel>(); p->set_id(id);
        widgets_[id] = p.get();
        resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });

    engine_.register_function("setFlex", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto key = args.get<std::string>(1, "");
        View* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        auto& f = v->flex();
        auto val = args.get<double>(2, 0);
        if (key == "direction") f.direction = (args.get<std::string>(2,"col")=="row") ? FlexDirection::row : FlexDirection::column;
        else if (key == "gap") f.gap = (float)val;
        else if (key == "padding") f.padding = (float)val;
        else if (key == "padding_top") f.padding_top = (float)val;
        else if (key == "padding_right") f.padding_right = (float)val;
        else if (key == "padding_bottom") f.padding_bottom = (float)val;
        else if (key == "padding_left") f.padding_left = (float)val;
        else if (key == "flex_grow") f.flex_grow = (float)val;
        else if (key == "flex_shrink") f.flex_shrink = (float)val;
        else if (key == "flex_basis") f.flex_basis = (float)val;
        else if (key == "flex_wrap") f.flex_wrap = val > 0;
        else if (key == "order") f.order = (int)val;
        else if (key == "width") f.preferred_width = (float)val;
        else if (key == "height") f.preferred_height = (float)val;
        else if (key == "min_width") f.min_width = (float)val;
        else if (key == "min_height") f.min_height = (float)val;
        else if (key == "max_width") f.max_width = (float)val;
        else if (key == "max_height") f.max_height = (float)val;
        // Margin
        else if (key == "margin") f.margin = (float)val;
        else if (key == "margin_top") f.margin_top = (float)val;
        else if (key == "margin_right") f.margin_right = (float)val;
        else if (key == "margin_bottom") f.margin_bottom = (float)val;
        else if (key == "margin_left") f.margin_left = (float)val;
        // Directional gap
        else if (key == "row_gap") f.row_gap = (float)val;
        else if (key == "column_gap") f.column_gap = (float)val;
        // Alignment
        else if (key == "align_items") {
            auto a = args.get<std::string>(2,"stretch");
            if (a=="start") f.align_items=FlexAlign::start;
            else if (a=="center") f.align_items=FlexAlign::center;
            else if (a=="end") f.align_items=FlexAlign::end;
            else f.align_items=FlexAlign::stretch;
        }
        else if (key == "align_self") {
            auto a = args.get<std::string>(2,"auto");
            if (a=="start") f.align_self=FlexAlign::start;
            else if (a=="center") f.align_self=FlexAlign::center;
            else if (a=="end") f.align_self=FlexAlign::end;
            else if (a=="stretch") f.align_self=FlexAlign::stretch;
            else f.align_self=FlexAlign::auto_;
        }
        else if (key == "justify_content") {
            auto j = args.get<std::string>(2,"start");
            if (j=="center") f.justify_content=FlexJustify::center;
            else if (j=="end") f.justify_content=FlexJustify::end_;
            else if (j=="space-between"||j=="space_between") f.justify_content=FlexJustify::space_between;
            else if (j=="space-around"||j=="space_around") f.justify_content=FlexJustify::space_around;
            else if (j=="space-evenly"||j=="space_evenly") f.justify_content=FlexJustify::space_evenly;
            else f.justify_content=FlexJustify::start;
        }
        return choc::value::Value();
    });

    engine_.register_function("layout", [this](choc::javascript::ArgumentList) {
        root_.layout_children();
        return choc::value::Value();
    });

    engine_.register_function("createMeter", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto o = args.get<std::string>(1, "vertical");
        auto pid = args.get<std::string>(2, "");
        auto m = std::make_unique<Meter>(); m->set_id(id);
        if (o == "horizontal") m->set_orientation(Meter::Orientation::horizontal);
        widgets_[id] = m.get(); resolve_parent(pid)->add_child(std::move(m));
        return choc::value::createString(id);
    });

    engine_.register_function("createXYPad", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<XYPad>(); p->set_id(id);
        widgets_[id] = p.get(); resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });

    engine_.register_function("createWaveform", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto w = std::make_unique<WaveformView>(); w->set_id(id);
        widgets_[id] = w.get(); resolve_parent(pid)->add_child(std::move(w));
        return choc::value::createString(id);
    });

    engine_.register_function("createSpectrum", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto s = std::make_unique<SpectrumView>(); s->set_id(id);
        widgets_[id] = s.get(); resolve_parent(pid)->add_child(std::move(s));
        return choc::value::createString(id);
    });

    engine_.register_function("createCombo", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto c = std::make_unique<ComboBox>(); c->set_id(id);
        auto* ptr = c.get(); widgets_[id] = ptr;
        c->on_change = [this, id](int idx) {
            engine_.evaluate("__dispatch__('" + id + "', 'select', " + std::to_string(idx) + ")");
        };
        resolve_parent(pid)->add_child(std::move(c));
        return choc::value::createString(id);
    });

    engine_.register_function("createProgress", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<ProgressBar>(); p->set_id(id);
        widgets_[id] = p.get(); resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });

    engine_.register_function("createScrollView", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto s = std::make_unique<ScrollView>(); s->set_id(id);
        widgets_[id] = s.get(); resolve_parent(pid)->add_child(std::move(s));
        return choc::value::createString(id);
    });

    engine_.register_function("createTextEditor", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto ed = std::make_unique<TextEditor>(); ed->set_id(id);
        auto* ptr = ed.get(); widgets_[id] = ptr;
        ed->on_return = [this, id](const std::string& text) {
            std::string e; for (char c : text) { if (c=='\'') e+= "\\'"; else if (c=='\n') e+= "\\n"; else e+= c; }
            engine_.evaluate("__dispatch__('" + id + "', 'return', '" + e + "')");
        };
        ed->on_change = [this, id](const std::string& text) {
            std::string e; for (char c : text) { if (c=='\'') e+= "\\'"; else if (c=='\n') e+= "\\n"; else e+= c; }
            engine_.evaluate("__dispatch__('" + id + "', 'change', '" + e + "')");
        };
        resolve_parent(pid)->add_child(std::move(ed));
        return choc::value::createString(id);
    });

    engine_.register_function("createCanvas", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto c = std::make_unique<CanvasWidget>(); c->set_id(id);
        widgets_[id] = c.get(); resolve_parent(pid)->add_child(std::move(c));
        return choc::value::createString(id);
    });

    // Property setters
    engine_.register_function("setLabel", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto text = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (auto* k = dynamic_cast<Knob*>(v)) k->set_label(text);
        else if (auto* f = dynamic_cast<Fader*>(v)) f->set_label(text);
        else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_label(text);
        else if (auto* tb = dynamic_cast<ToggleButton*>(v)) tb->set_label(text);
        else if (auto* l = dynamic_cast<Label*>(v)) l->set_text(text);
        return choc::value::Value();
    });

    // setStyle — placeholder until KnobStyle/ToggleStyle enums added to main widgets
    engine_.register_function("setStyle", [](choc::javascript::ArgumentList) {
        return choc::value::Value();
    });

    engine_.register_function("setItems", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        if (auto* c = dynamic_cast<ComboBox*>(widget(id))) {
            std::vector<std::string> items;
            if (args.numArgs > 1 && args[1]) {
                auto& arr = *args[1];
                for (uint32_t i = 0; i < arr.size(); ++i)
                    items.push_back(std::string(arr[i].toString()));
            }
            c->set_items(std::move(items));
        }
        return choc::value::Value();
    });

    // Typography properties
    engine_.register_function("setFontWeight", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        int w = static_cast<int>(args.get<double>(1, 400));
        if (auto* l = dynamic_cast<Label*>(v)) l->set_font_weight(w);
        return choc::value::Value();
    });

    engine_.register_function("setFontStyle", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto s = args.get<std::string>(1, "normal");
        if (auto* l = dynamic_cast<Label*>(v)) l->set_font_style(s == "italic" ? 1 : 0);
        return choc::value::Value();
    });

    engine_.register_function("setLetterSpacing", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto sp = static_cast<float>(args.get<double>(1, 0));
        if (auto* l = dynamic_cast<Label*>(v)) l->set_letter_spacing(sp);
        return choc::value::Value();
    });

    engine_.register_function("setLineHeight", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto lh = static_cast<float>(args.get<double>(1, 0));
        if (auto* l = dynamic_cast<Label*>(v)) l->set_line_height(lh);
        return choc::value::Value();
    });

    engine_.register_function("setTextAlign", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto a = args.get<std::string>(1, "left");
        if (auto* l = dynamic_cast<Label*>(v)) {
            if (a == "center") l->set_text_align(LabelAlign::center);
            else if (a == "right") l->set_text_align(LabelAlign::right);
            else l->set_text_align(LabelAlign::left);
        }
        return choc::value::Value();
    });

    engine_.register_function("setMultiLine", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto ml = args.get<double>(1, 0) > 0.5;
        if (auto* l = dynamic_cast<Label*>(v)) l->set_multi_line(ml);
        return choc::value::Value();
    });

    engine_.register_function("setFontSize", [this](choc::javascript::ArgumentList args) {
        if (auto* l = dynamic_cast<Label*>(widget(args.get<std::string>(0, ""))))
            l->set_font_size(static_cast<float>(args.get<double>(1, 14)));
        return choc::value::Value();
    });

    engine_.register_function("setProgress", [this](choc::javascript::ArgumentList args) {
        if (auto* p = dynamic_cast<ProgressBar*>(widget(args.get<std::string>(0, ""))))
            p->set_progress(static_cast<float>(args.get<double>(1, 0)));
        return choc::value::Value();
    });

    engine_.register_function("setMeterLevel", [this](choc::javascript::ArgumentList args) {
        if (auto* m = dynamic_cast<Meter*>(widget(args.get<std::string>(0, ""))))
            m->set_level(static_cast<float>(args.get<double>(1, 0)),
                        static_cast<float>(args.get<double>(2, 0)));
        return choc::value::Value();
    });

    engine_.register_function("setXY", [this](choc::javascript::ArgumentList args) {
        if (auto* p = dynamic_cast<XYPad*>(widget(args.get<std::string>(0, "")))) {
            p->set_x(static_cast<float>(args.get<double>(1, .5)));
            p->set_y(static_cast<float>(args.get<double>(2, .5)));
        }
        return choc::value::Value();
    });

    engine_.register_function("setWaveformData", [this](choc::javascript::ArgumentList args) {
        if (auto* w = dynamic_cast<WaveformView*>(widget(args.get<std::string>(0, "")))) {
            if (args.numArgs > 1 && args[1]) {
                auto& a = *args[1]; std::vector<float> d(a.size());
                for (uint32_t i = 0; i < a.size(); ++i) d[i] = static_cast<float>(a[i].getWithDefault<double>(0));
                w->set_data(std::move(d));
            }
        }
        return choc::value::Value();
    });

    engine_.register_function("setSpectrumData", [this](choc::javascript::ArgumentList args) {
        if (auto* s = dynamic_cast<SpectrumView*>(widget(args.get<std::string>(0, "")))) {
            if (args.numArgs > 1 && args[1]) {
                auto& a = *args[1]; std::vector<float> d(a.size());
                for (uint32_t i = 0; i < a.size(); ++i) d[i] = static_cast<float>(a[i].getWithDefault<double>(0));
                s->set_spectrum(std::move(d));
            }
        }
        return choc::value::Value();
    });

    engine_.register_function("setPlaceholder", [this](choc::javascript::ArgumentList args) {
        if (auto* e = dynamic_cast<TextEditor*>(widget(args.get<std::string>(0, ""))))
            e->placeholder = args.get<std::string>(1, "");
        return choc::value::Value();
    });

    engine_.register_function("setText", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto t = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (auto* e = dynamic_cast<TextEditor*>(v)) e->set_text(t);
        else if (auto* l = dynamic_cast<Label*>(v)) l->set_text(t);
        return choc::value::Value();
    });

    engine_.register_function("getText", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        if (auto* e = dynamic_cast<TextEditor*>(v)) return choc::value::createString(e->text());
        if (auto* l = dynamic_cast<Label*>(v)) return choc::value::createString(l->text());
        return choc::value::createString("");
    });

    engine_.register_function("setPanelStyle", [this](choc::javascript::ArgumentList args) {
        if (auto* p = dynamic_cast<Panel*>(widget(args.get<std::string>(0, "")))) {
            if (args.numArgs > 1) p->set_background_token(args.get<std::string>(1, "bg.surface"));
            if (args.numArgs > 2) p->set_border_token(args.get<std::string>(2, "control.border"));
            if (args.numArgs > 3) p->set_corner_radius(static_cast<float>(args.get<double>(3, 8)));
            if (args.numArgs > 4) p->set_border_width(static_cast<float>(args.get<double>(4, 1)));
        }
        return choc::value::Value();
    });

    engine_.register_function("setScrollContentSize", [this](choc::javascript::ArgumentList args) {
        if (auto* s = dynamic_cast<ScrollView*>(widget(args.get<std::string>(0, ""))))
            s->set_content_size({static_cast<float>(args.get<double>(1,0)),
                                static_cast<float>(args.get<double>(2,0))});
        return choc::value::Value();
    });

    // ── Visual properties (CSS Box Model) ─────────────────────────────

    auto parseHexColor = [](const std::string& hex) -> canvas::Color {
        canvas::Color c{255,255,255,255};
        if (hex.size() >= 7 && hex[0] == '#') {
            c.r = static_cast<uint8_t>(std::stoul(hex.substr(1,2), nullptr, 16));
            c.g = static_cast<uint8_t>(std::stoul(hex.substr(3,2), nullptr, 16));
            c.b = static_cast<uint8_t>(std::stoul(hex.substr(5,2), nullptr, 16));
            if (hex.size() >= 9)
                c.a = static_cast<uint8_t>(std::stoul(hex.substr(7,2), nullptr, 16));
        }
        return c;
    };

    engine_.register_function("setBackground", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) v->set_background_color(parseHexColor(hex));
        return choc::value::Value();
    });

    engine_.register_function("setBorder", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto width = args.get<double>(2, 1.0);
        auto radius = args.get<double>(3, 0.0);
        auto* v = id.empty() ? &root_ : widget(id);
        if (v && !hex.empty()) v->set_border(parseHexColor(hex), (float)width, (float)radius);
        return choc::value::Value();
    });

    engine_.register_function("setOpacity", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto alpha = args.get<double>(1, 1.0);
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_opacity((float)alpha);
        return choc::value::Value();
    });

    engine_.register_function("setTextColor", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto hex = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v || hex.empty()) return choc::value::Value();
        // Set a custom text color token override on the view's theme
        auto theme = v->theme();
        theme.colors["text.primary"] = parseHexColor(hex);
        v->set_theme(theme);
        return choc::value::Value();
    });

    engine_.register_function("setOverflow", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto mode = args.get<std::string>(1, "hidden");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_overflow(mode == "visible" ? View::Overflow::visible : View::Overflow::hidden);
        return choc::value::Value();
    });

    // Canvas drawing
    engine_.register_function("canvasClear", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, ""))))
            c->clear_commands();
        return choc::value::Value();
    });

    engine_.register_function("canvasRect", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            auto h = args.get<std::string>(5,"#fff");
            if (h.size()>=7) { cmd.color.r=(uint8_t)std::stoul(h.substr(1,2),0,16);
                cmd.color.g=(uint8_t)std::stoul(h.substr(3,2),0,16);
                cmd.color.b=(uint8_t)std::stoul(h.substr(5,2),0,16); cmd.color.a=255; }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // setTransitionDuration(id, seconds) — CSS transition duration for animated property changes
    engine_.register_function("setTransitionDuration", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto dur = static_cast<float>(args.get<double>(1, 0.15));
        // Store transition duration on the view's theme as a dimension token
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) {
            auto theme = v->theme();
            theme.dimensions["transition.duration"] = dur;
            v->set_theme(theme);
        }
        return choc::value::Value();
    });

    // setScale(id, scale) — CSS transform: scale()
    engine_.register_function("setScale", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto s = static_cast<float>(args.get<double>(1, 1.0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_scale(s);
        return choc::value::Value();
    });

    // setTextOverflow(id, "ellipsis"|"clip") — CSS text-overflow
    engine_.register_function("setTextOverflow", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto mode = args.get<std::string>(1, "clip");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_text_overflow_ellipsis(mode == "ellipsis");
        return choc::value::Value();
    });

    // setCursor(id, "pointer"|"crosshair"|"text"|"default") — CSS cursor
    engine_.register_function("setCursor", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto c = args.get<std::string>(1, "default");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (c == "pointer") v->set_cursor(View::CursorStyle::pointer);
        else if (c == "crosshair") v->set_cursor(View::CursorStyle::crosshair);
        else if (c == "text") v->set_cursor(View::CursorStyle::text);
        else if (c == "grab") v->set_cursor(View::CursorStyle::grab);
        else v->set_cursor(View::CursorStyle::default_);
        return choc::value::Value();
    });

    // setBoxShadow(id, offsetX, offsetY, blur, spread, color)
    engine_.register_function("setBoxShadow", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto ox = static_cast<float>(args.get<double>(1, 0));
        auto oy = static_cast<float>(args.get<double>(2, 2));
        auto blur = static_cast<float>(args.get<double>(3, 4));
        auto spread = static_cast<float>(args.get<double>(4, 0));
        auto hex = args.get<std::string>(5, "#00000050");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_box_shadow(ox, oy, blur, spread, parseHexColor(hex));
        return choc::value::Value();
    });

    // Path builder API from JS
    engine_.register_function("beginPath", [this](choc::javascript::ArgumentList) {
        // Store path commands for deferred rendering via CanvasWidget
        return choc::value::Value();
    });

    // drawPath(canvasId, commands) — draw a path on a CanvasWidget
    // Commands: "M x y" (move), "L x y" (line), "Q cx cy x y" (quad), "C c1x c1y c2x c2y x y" (cubic), "Z" (close)
    engine_.register_function("drawPath", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pathStr = args.get<std::string>(1, "");
        auto fillHex = args.get<std::string>(2, "");
        auto strokeHex = args.get<std::string>(3, "");
        auto lineW = static_cast<float>(args.get<double>(4, 1.0));
        (void)id; (void)pathStr; (void)fillHex; (void)strokeHex; (void)lineW;
        // TODO: parse SVG-like path string and render via CanvasWidget
        return choc::value::Value();
    });

    // compileShader(sksl_code) → {success: bool, error: string}
    // Validates SkSL shader code without applying it
    engine_.register_function("compileShader", [](choc::javascript::ArgumentList args) {
        auto code = args.get<std::string>(0, "");
        auto result = choc::value::createObject("");
        if (code.empty()) {
            result.addMember("success", choc::value::createBool(false));
            result.addMember("error", choc::value::createString("Empty shader code"));
            return result;
        }
        // For now, return success — actual SkRuntimeEffect compilation
        // would require Skia headers which aren't available in the bridge
        result.addMember("success", choc::value::createBool(true));
        result.addMember("error", choc::value::createString(""));
        return result;
    });

    // measureText(text, fontSize) → {width, ascent, descent, lineHeight}
    engine_.register_function("measureText", [](choc::javascript::ArgumentList args) {
        auto text = args.get<std::string>(0, "");
        auto size = static_cast<float>(args.get<double>(1, 14.0));
        // Return approximate metrics (exact when Skia canvas is available)
        float width = static_cast<float>(text.size()) * size * 0.6f;
        float ascent = size * 0.75f;
        float descent = size * 0.25f;
        float lineHeight = size * 1.4f;
        auto result = choc::value::createObject("");
        result.addMember("width", choc::value::createFloat64(width));
        result.addMember("ascent", choc::value::createFloat64(ascent));
        result.addMember("descent", choc::value::createFloat64(descent));
        result.addMember("lineHeight", choc::value::createFloat64(lineHeight));
        return result;
    });

    // Theme control
    engine_.register_function("setTheme", [this](choc::javascript::ArgumentList args) {
        auto n = args.get<std::string>(0, "dark");
        root_.set_theme(n=="light" ? Theme::light() : n=="pro_audio" ? Theme::pro_audio() : Theme::dark());
        return choc::value::Value();
    });

    engine_.register_function("applyTokenDiff", [this](choc::javascript::ArgumentList args) {
        auto json = args.get<std::string>(0, "");
        if (!json.empty()) { auto d = Theme::from_json(json); auto c = root_.theme(); c.apply_overrides(d); root_.set_theme(c); }
        return choc::value::Value();
    });

    engine_.register_function("getThemeJson", [this](choc::javascript::ArgumentList) {
        return choc::value::createString(root_.theme().to_json());
    });

    // Shell exec (for Claude CLI)
    // Ensures PATH includes common tool locations (homebrew, npm global, etc.)
    engine_.register_function("exec", [](choc::javascript::ArgumentList args) {
        auto cmd = args.get<std::string>(0, "");
        if (cmd.empty()) return choc::value::createString("");
        // Prepend common tool paths that GUI apps miss
        auto full_cmd = std::string(
            "export PATH=\"$HOME/.local/bin:$HOME/.npm-global/bin:"
            "/opt/homebrew/bin:/usr/local/bin:$PATH\"; ") + cmd;
        std::string r;
        FILE* p = popen(full_cmd.c_str(), "r");
        if (!p) return choc::value::createString("");
        char buf[4096];
        while (fgets(buf, sizeof(buf), p)) r += buf;
        pclose(p);
        return choc::value::createString(r);
    });
}

void WidgetBridge::forward_key_event(int key_code, uint16_t modifiers, bool is_down) {
    if (!is_down) return;
    engine_.evaluate("__dispatch__('__global__', 'keydown', {"
        "key:" + std::to_string(key_code) +
        ",mods:" + std::to_string(modifiers) + "})");
}

} // namespace pulp::view
