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
        else if (key == "flex_wrap") f.flex_wrap = val > 0;
        else if (key == "width") f.preferred_width = (float)val;
        else if (key == "height") f.preferred_height = (float)val;
        else if (key == "min_width") f.min_width = (float)val;
        else if (key == "min_height") f.min_height = (float)val;
        else if (key == "max_width") f.max_width = (float)val;
        else if (key == "max_height") f.max_height = (float)val;
        else if (key == "align_items") {
            auto a = args.get<std::string>(2,"stretch");
            if (a=="start") f.align_items=FlexAlign::start;
            else if (a=="center") f.align_items=FlexAlign::center;
            else if (a=="end") f.align_items=FlexAlign::end;
            else f.align_items=FlexAlign::stretch;
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
    engine_.register_function("exec", [](choc::javascript::ArgumentList args) {
        auto cmd = args.get<std::string>(0, "");
        if (cmd.empty()) return choc::value::createString("");
        std::string r; FILE* p = popen(cmd.c_str(), "r"); if (!p) return choc::value::createString("");
        char buf[4096]; while (fgets(buf, sizeof(buf), p)) r += buf; pclose(p);
        return choc::value::createString(r);
    });
}

} // namespace pulp::view
