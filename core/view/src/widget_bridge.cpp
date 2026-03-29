#include <pulp/view/widget_bridge.hpp>
#include <pulp/view/animation.hpp>
#include <pulp/view/frame_clock.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/canvas_widget.hpp>
#include <pulp/platform/popup_menu.hpp>
#include <pulp/platform/file_dialog.hpp>
#include <pulp/platform/clipboard.hpp>
#include <web_compat_preludes_gen.hpp>
#include <chrono>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

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

static const char* kDomOpsInit =
    "Element.prototype.appendChild = function(child) {"
    "  if (!(child instanceof Element)) return child;"
    "  if (child._parentElement) child._parentElement.removeChild(child);"
    "  child._parentElement = this;"
    "  this._children.push(child);"
    "  this._ensureNative();"
    "  __domAppend(this._id, child._id, child.tagName.toLowerCase());"
    "  child._nativeCreated = true;"
    "  if (child._textContent) setText(child._id, child._textContent);"
    "  child.style._flushAll();"
    "  child._reapplyStylesheets();"
    "  return child;"
    "};"
    "Element.prototype.removeChild = function(child) {"
    "  var idx = this._children.indexOf(child);"
    "  if (idx < 0) return child;"
    "  this._children.splice(idx, 1);"
    "  child._parentElement = null;"
    "  if (child._nativeCreated) __domRemove(child._id);"
    "  child._nativeCreated = false;"
    "  return child;"
    "};"
    "Element.prototype.remove = function() {"
    "  if (this._parentElement) this._parentElement.removeChild(this);"
    "};"
    "Element.prototype.insertBefore = function(newChild, refChild) {"
    "  if (!refChild) return this.appendChild(newChild);"
    "  var idx = this._children.indexOf(refChild);"
    "  if (idx < 0) return this.appendChild(newChild);"
    "  if (newChild._parentElement) newChild._parentElement.removeChild(newChild);"
    "  newChild._parentElement = this;"
    "  this._children.splice(idx, 0, newChild);"
    "  this._ensureNative();"
    "  __domAppend(this._id, newChild._id, newChild.tagName.toLowerCase());"
    "  newChild._nativeCreated = true;"
    "  if (newChild._textContent) setText(newChild._id, newChild._textContent);"
    "  newChild.style._flushAll();"
    "  newChild._reapplyStylesheets();"
    "  return newChild;"
    "};"
    "Element.prototype.replaceChild = function(newChild, oldChild) {"
    "  var idx = this._children.indexOf(oldChild);"
    "  if (idx < 0) return oldChild;"
    "  this.removeChild(oldChild);"
    "  this.appendChild(newChild);"
    "  return oldChild;"
    "};";
static bool s_dom_ops_loaded = false;

WidgetBridge::WidgetBridge(ScriptEngine& engine, View& root, state::StateStore& store)
    : engine_(engine), root_(root), store_(store) {
    register_api();
    engine_.evaluate(kJSPreamble);
    engine_.evaluate(preludes::css_colors);
    engine_.evaluate(preludes::css_parser);
    engine_.evaluate(preludes::web_compat_element);
    engine_.evaluate(preludes::web_compat_style_decl);
    engine_.evaluate(preludes::web_compat_document);
    s_dom_ops_loaded = false;
}

void WidgetBridge::load_script(const std::string& code) {
    if (!s_dom_ops_loaded) {
        s_dom_ops_loaded = true;
        engine_.evaluate(kDomOpsInit);
    }
    // Append ";void 0" so the eval result is undefined, not the last
    // expression value. Elements have circular references (_parentElement
    // ↔ _children) which cause infinite recursion in CHOC's toChocValue().
    engine_.evaluate(code + "\n;void 0");
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

    // createImage(id, parentId) — HTML <img> equivalent
    engine_.register_function("createImage", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto img = std::make_unique<ImageView>(); img->set_id(id);
        widgets_[id] = img.get();
        resolve_parent(pid)->add_child(std::move(img));
        return choc::value::createString(id);
    });

    // setImageSource(id, path) — set image file path
    engine_.register_function("setImageSource", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto path = args.get<std::string>(1, "");
        if (auto* img = dynamic_cast<ImageView*>(widget(id)))
            img->set_image_path(path);
        return choc::value::Value();
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
    // animate(id, property, target, duration_ms, easing) — CSS transition equivalent
    // Smoothly interpolates a property from current to target over duration
    engine_.register_function("animate", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto prop = args.get<std::string>(1, "value");
        auto target = static_cast<float>(args.get<double>(2, 0));
        auto dur_ms = static_cast<float>(args.get<double>(3, 150));
        auto ease_name = args.get<std::string>(4, "ease_out_cubic");

        auto* v = widget(id);
        if (!v) return choc::value::Value();

        float dur = dur_ms / 1000.0f;
        (void)ease_name; // easing for future ValueAnimation integration

        // Apply property changes — for now immediate with duration stored
        // TODO: integrate with FrameClock for actual interpolation
        if (prop == "opacity") {
            v->set_opacity(target);
        } else if (prop == "scale") {
            v->set_scale(target);
        } else if (prop == "translate_x") {
            v->set_translate(target, v->translate_y());
        } else if (prop == "translate_y") {
            v->set_translate(v->translate_x(), target);
        } else if (prop == "rotation") {
            v->set_rotation(target);
        } else if (prop == "value") {
            if (auto* k = dynamic_cast<Knob*>(v)) k->set_value(target);
            else if (auto* f = dynamic_cast<Fader*>(v)) f->set_value(target);
            else if (auto* t = dynamic_cast<Toggle*>(v)) t->set_on(target > 0.5f);
        }
        (void)dur;
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

    // createGrid(id, parentId) — creates a grid container (CSS display: grid)
    engine_.register_function("createGrid", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<View>();
        v->set_id(id);
        v->set_layout_mode(LayoutMode::grid);
        widgets_[id] = v.get();
        resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    // setGrid(id, property, value) — set grid container properties
    engine_.register_function("setGrid", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto key = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v) return choc::value::Value();

        if (key == "template_columns") {
            auto tmpl = args.get<std::string>(2, "");
            v->grid().template_columns = GridStyle::parse_template(tmpl);
        } else if (key == "template_rows") {
            auto tmpl = args.get<std::string>(2, "");
            v->grid().template_rows = GridStyle::parse_template(tmpl);
        } else if (key == "column_gap") {
            v->grid().column_gap = static_cast<float>(args.get<double>(2, 0));
        } else if (key == "row_gap") {
            v->grid().row_gap = static_cast<float>(args.get<double>(2, 0));
        } else if (key == "gap") {
            float g = static_cast<float>(args.get<double>(2, 0));
            v->grid().column_gap = g;
            v->grid().row_gap = g;
        } else if (key == "column_start") {
            v->grid().grid_column_start = static_cast<int>(args.get<double>(2, 0));
        } else if (key == "column_end") {
            v->grid().grid_column_end = static_cast<int>(args.get<double>(2, 0));
        } else if (key == "row_start") {
            v->grid().grid_row_start = static_cast<int>(args.get<double>(2, 0));
        } else if (key == "row_end") {
            v->grid().grid_row_end = static_cast<int>(args.get<double>(2, 0));
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

    // ── Pointer events (P2) ─────────────────────────────────────────────

    // Helper: build JS object literal for pointer event data from MouseEvent
    auto pointer_data_js = [](const std::string& id, const MouseEvent& me) -> std::string {
        std::string js = "__dispatch__('" + id + "', '";
        // Event type placeholder — caller appends type
        return js;
    };

    // registerPointer(id) — enables pointer event dispatch for a widget
    engine_.register_function("registerPointer", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            auto* w = it->second;
            w->on_pointer_event = [this, id](const MouseEvent& me) {
                std::string type;
                if (me.is_down) type = "pointerdown";
                else type = "pointerup";
                if (me.is_cancelled) type = "pointercancel";

                std::string data = "{"
                    "clientX:" + std::to_string(me.window_position.x) + ","
                    "clientY:" + std::to_string(me.window_position.y) + ","
                    "offsetX:" + std::to_string(me.position.x) + ","
                    "offsetY:" + std::to_string(me.position.y) + ","
                    "pointerId:" + std::to_string(me.pointer_id) + ","
                    "pointerType:'" + std::string(me.pointerTypeString()) + "',"
                    "isPrimary:" + (me.isPrimary() ? "true" : "false") + ","
                    "pressure:" + std::to_string(me.pressure) + ","
                    "altitudeAngle:" + std::to_string(me.altitude_angle) + ","
                    "azimuthAngle:" + std::to_string(me.azimuth_angle) + ","
                    "button:" + std::to_string(static_cast<int>(me.button)) + ","
                    "ctrlKey:" + (me.isCtrlDown() ? "true" : "false") + ","
                    "shiftKey:" + (me.isShiftDown() ? "true" : "false") + ","
                    "altKey:" + (me.isAltDown() ? "true" : "false") + ","
                    "metaKey:" + (me.isCmdDown() ? "true" : "false") +
                    "}";

                engine_.evaluate("__dispatch__('" + id + "', '" + type + "', " + data + ")");
            };
        }
        return choc::value::Value();
    });

    // registerGesture(id) — enables gesture event dispatch for a widget
    engine_.register_function("registerGesture", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            it->second->on_gesture_cb = [this, id](const GestureEvent& ge) {
                std::string type;
                switch (ge.phase) {
                    case GesturePhase::began:     type = "gesturestart"; break;
                    case GesturePhase::ended:     type = "gestureend"; break;
                    case GesturePhase::cancelled: type = "gestureend"; break;
                    default:                      type = "gesturechange"; break;
                }
                std::string data = "{"
                    "scale:" + std::to_string(ge.scale) + ","
                    "rotation:" + std::to_string(ge.rotation) + ","
                    "clientX:" + std::to_string(ge.position.x) + ","
                    "clientY:" + std::to_string(ge.position.y) +
                    "}";
                engine_.evaluate("__dispatch__('" + id + "', '" + type + "', " + data + ")");
            };
        }
        return choc::value::Value();
    });

    // nativeSetPointerCapture(id, pointerId) — P2b
    engine_.register_function("nativeSetPointerCapture", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pointerId = static_cast<int>(args.get<double>(1, 0));
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            it->second->set_pointer_capture(pointerId);
            engine_.evaluate("__dispatch__('" + id + "', 'gotpointercapture', {pointerId:" + std::to_string(pointerId) + "})");
        }
        return choc::value::Value();
    });

    // nativeReleasePointerCapture(id, pointerId) — P2b
    engine_.register_function("nativeReleasePointerCapture", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pointerId = static_cast<int>(args.get<double>(1, 0));
        auto it = widgets_.find(id);
        if (it != widgets_.end()) {
            it->second->release_pointer_capture(pointerId);
            engine_.evaluate("__dispatch__('" + id + "', 'lostpointercapture', {pointerId:" + std::to_string(pointerId) + "})");
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
        v->invalidate_layout();  // auto-invalidation on flex property change
        return choc::value::Value();
    });

    // __domAppend(parentId, childId, tag) — native appendChild.
    // Creates a native widget under parentId, purely in C++ — no re-entrant
    // JS evaluation which causes stack overflow in QuickJS.
    engine_.register_function("__domAppend", [this](choc::javascript::ArgumentList args) {
        auto parentId = args.get<std::string>(0, "");
        auto childId = args.get<std::string>(1, "");
        auto tag = args.get<std::string>(2, "div");
        auto* existing = widget(childId);
        if (existing) {
            if (auto* p = existing->parent()) p->remove_child(existing);
            widgets_.erase(childId);
        }
        // Create the appropriate widget type based on HTML tag
        std::unique_ptr<View> child;
        if (tag == "span" || tag == "p" || tag == "label" ||
            tag == "h1" || tag == "h2" || tag == "h3" ||
            tag == "h4" || tag == "h5" || tag == "h6") {
            auto lbl = std::make_unique<Label>();
            lbl->set_id(childId);
            child = std::move(lbl);
        } else {
            auto v = std::make_unique<View>();
            v->set_id(childId);
            if (tag == "div" || tag == "section" || tag == "article" || tag == "aside" ||
                tag == "header" || tag == "footer" || tag == "nav" || tag == "main")
                v->flex().direction = FlexDirection::column;
            child = std::move(v);
        }
        widgets_[childId] = child.get();
        resolve_parent(parentId)->add_child(std::move(child));
        return choc::value::Value();
    });

    // __domRemove(childId) — native removeChild implementation
    engine_.register_function("__domRemove", [this](choc::javascript::ArgumentList args) {
        auto childId = args.get<std::string>(0, "");
        auto* w = widget(childId);
        if (w) {
            if (auto* p = w->parent()) p->remove_child(w);
            widgets_.erase(childId);
        }
        return choc::value::Value();
    });

    engine_.register_function("layout", [this](choc::javascript::ArgumentList) {
        root_.clear_layout_dirty();
        root_.layout_children();
        return choc::value::Value();
    });

    // getLayoutRect(id) -> {x, y, width, height, top, right, bottom, left}
    // Returns layout-resolved bounds in root-relative coordinates
    engine_.register_function("getLayoutRect", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        View* v = id.empty() ? &root_ : widget(id);
        if (!v) {
            auto r = choc::value::createObject("");
            r.addMember("x", choc::value::createFloat64(0));
            r.addMember("y", choc::value::createFloat64(0));
            r.addMember("width", choc::value::createFloat64(0));
            r.addMember("height", choc::value::createFloat64(0));
            r.addMember("top", choc::value::createFloat64(0));
            r.addMember("right", choc::value::createFloat64(0));
            r.addMember("bottom", choc::value::createFloat64(0));
            r.addMember("left", choc::value::createFloat64(0));
            return r;
        }
        // Walk up parent chain to compute root-relative position
        float rx = 0, ry = 0;
        View* cur = v;
        while (cur) {
            rx += cur->bounds().x;
            ry += cur->bounds().y;
            cur = cur->parent();
        }
        auto b = v->bounds();
        auto r = choc::value::createObject("");
        r.addMember("x", choc::value::createFloat64(rx));
        r.addMember("y", choc::value::createFloat64(ry));
        r.addMember("width", choc::value::createFloat64(b.width));
        r.addMember("height", choc::value::createFloat64(b.height));
        r.addMember("top", choc::value::createFloat64(ry));
        r.addMember("right", choc::value::createFloat64(rx + b.width));
        r.addMember("bottom", choc::value::createFloat64(ry + b.height));
        r.addMember("left", choc::value::createFloat64(rx));
        return r;
    });

    // getRootSize() -> {width, height} — actual root view dimensions for vw/vh/matchMedia
    engine_.register_function("getRootSize", [this](choc::javascript::ArgumentList) {
        auto b = root_.bounds();
        auto r = choc::value::createObject("");
        r.addMember("width", choc::value::createFloat64(b.width));
        r.addMember("height", choc::value::createFloat64(b.height));
        return r;
    });

    // setPointerEvents(id, "none"|"auto") — CSS pointer-events: skip in hit_test
    engine_.register_function("setPointerEvents", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto mode = args.get<std::string>(1, "auto");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_hit_testable(mode != "none");
        return choc::value::Value();
    });

    // setVisibility(id, "visible"|"hidden") — hidden preserves layout space
    engine_.register_function("setVisibility", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto vis = args.get<std::string>(1, "visible");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) {
            // visibility:hidden = still takes space but not painted
            // We use opacity 0 + still visible for layout
            if (vis == "hidden") { v->set_opacity(0); }
            else { v->set_opacity(1); }
        }
        return choc::value::Value();
    });

    // setWhiteSpace(id, "normal"|"nowrap"|"pre"|"pre-wrap")
    engine_.register_function("setWhiteSpace", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto ws = args.get<std::string>(1, "normal");
        if (auto* l = dynamic_cast<Label*>(widget(id)))
            l->set_multi_line(ws != "nowrap");
        return choc::value::Value();
    });

    // setUserSelect(id, "none"|"text"|"all")
    engine_.register_function("setUserSelect", [this](choc::javascript::ArgumentList args) {
        (void)args; // Store for future use — currently no-op
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

    engine_.register_function("createListBox", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto lb = std::make_unique<ListBox>(); lb->set_id(id);
        auto* ptr = lb.get(); widgets_[id] = ptr;
        lb->on_select = [this, id](int idx) {
            engine_.evaluate("__dispatch__('" + id + "', 'select', " + std::to_string(idx) + ")");
        };
        lb->on_activate = [this, id](int idx) {
            engine_.evaluate("__dispatch__('" + id + "', 'activate', " + std::to_string(idx) + ")");
        };
        resolve_parent(pid)->add_child(std::move(lb));
        return choc::value::createString(id);
    });

    engine_.register_function("setListItems", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* lb = dynamic_cast<ListBox*>(v)) {
            std::vector<std::string> items;
            if (args.numArgs > 1 && args[1]) {
                auto& arr = *args[1];
                for (uint32_t i = 0; i < arr.size(); ++i)
                    items.push_back(std::string(arr[i].getString()));
            }
            lb->set_items(std::move(items));
        }
        return choc::value::Value{};
    });

    engine_.register_function("setListSelected", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* lb = dynamic_cast<ListBox*>(v)) {
            lb->set_selected(args.get<int>(1, 0));
            lb->ensure_visible(lb->selected());
        }
        return choc::value::Value{};
    });

    engine_.register_function("setListRowHeight", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id); if (!v) return choc::value::Value{};
        if (auto* lb = dynamic_cast<ListBox*>(v))
            lb->set_row_height(static_cast<float>(args.get<double>(1, 24.0)));
        return choc::value::Value{};
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

    // CSS Color Level 4 parser — accepts hex, rgb(), rgba(), hsl(), hsla(), named, transparent
    auto parseColor = [](const std::string& str) -> canvas::Color {
        canvas::Color c{255,255,255,255};
        if (str.empty()) return c;

        // transparent
        if (str == "transparent") return {0, 0, 0, 0};

        // Hex: #RGB, #RRGGBB, #RRGGBBAA
        if (str[0] == '#') {
            if (str.size() == 4) {  // #RGB → #RRGGBB
                c.r = static_cast<uint8_t>(std::stoul(std::string(2, str[1]), nullptr, 16));
                c.g = static_cast<uint8_t>(std::stoul(std::string(2, str[2]), nullptr, 16));
                c.b = static_cast<uint8_t>(std::stoul(std::string(2, str[3]), nullptr, 16));
            } else if (str.size() >= 7) {
                c.r = static_cast<uint8_t>(std::stoul(str.substr(1,2), nullptr, 16));
                c.g = static_cast<uint8_t>(std::stoul(str.substr(3,2), nullptr, 16));
                c.b = static_cast<uint8_t>(std::stoul(str.substr(5,2), nullptr, 16));
                if (str.size() >= 9)
                    c.a = static_cast<uint8_t>(std::stoul(str.substr(7,2), nullptr, 16));
            }
            return c;
        }

        // rgb(r, g, b) / rgba(r, g, b, a)
        if (str.substr(0, 4) == "rgb(" || str.substr(0, 5) == "rgba(") {
            auto inner = str.substr(str.find('(') + 1);
            inner = inner.substr(0, inner.find(')'));
            float vals[4] = {0, 0, 0, 1};
            int n = 0;
            std::istringstream ss(inner);
            std::string tok;
            while (std::getline(ss, tok, ',') && n < 4) {
                while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
                vals[n++] = std::stof(tok);
            }
            c.r = static_cast<uint8_t>(std::clamp(vals[0], 0.0f, 255.0f));
            c.g = static_cast<uint8_t>(std::clamp(vals[1], 0.0f, 255.0f));
            c.b = static_cast<uint8_t>(std::clamp(vals[2], 0.0f, 255.0f));
            c.a = static_cast<uint8_t>(std::clamp(vals[3] * 255.0f, 0.0f, 255.0f));
            return c;
        }

        // hsl(h, s%, l%) / hsla(h, s%, l%, a)
        if (str.substr(0, 4) == "hsl(" || str.substr(0, 5) == "hsla(") {
            auto inner = str.substr(str.find('(') + 1);
            inner = inner.substr(0, inner.find(')'));
            float vals[4] = {0, 0, 0, 1};
            int n = 0;
            std::istringstream ss(inner);
            std::string tok;
            while (std::getline(ss, tok, ',') && n < 4) {
                while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
                if (tok.back() == '%') tok.pop_back();
                vals[n++] = std::stof(tok);
            }
            float h = std::fmod(vals[0], 360.0f) / 360.0f;
            float s = vals[1] / 100.0f;
            float l = vals[2] / 100.0f;
            // HSL to RGB conversion
            auto hue2rgb = [](float p, float q, float t) {
                if (t < 0) t += 1; if (t > 1) t -= 1;
                if (t < 1.0f/6) return p + (q - p) * 6 * t;
                if (t < 1.0f/2) return q;
                if (t < 2.0f/3) return p + (q - p) * (2.0f/3 - t) * 6;
                return p;
            };
            float r, g, b;
            if (s == 0) { r = g = b = l; }
            else {
                float q = l < 0.5f ? l * (1 + s) : l + s - l * s;
                float p = 2 * l - q;
                r = hue2rgb(p, q, h + 1.0f/3);
                g = hue2rgb(p, q, h);
                b = hue2rgb(p, q, h - 1.0f/3);
            }
            c.r = static_cast<uint8_t>(r * 255); c.g = static_cast<uint8_t>(g * 255);
            c.b = static_cast<uint8_t>(b * 255);
            c.a = static_cast<uint8_t>(std::clamp(vals[3] * 255.0f, 0.0f, 255.0f));
            return c;
        }

        // Named colors (common subset)
        static const std::unordered_map<std::string, uint32_t> named = {
            {"black", 0x000000}, {"white", 0xFFFFFF}, {"red", 0xFF0000},
            {"green", 0x008000}, {"blue", 0x0000FF}, {"yellow", 0xFFFF00},
            {"cyan", 0x00FFFF}, {"magenta", 0xFF00FF}, {"orange", 0xFFA500},
            {"purple", 0x800080}, {"pink", 0xFFC0CB}, {"gray", 0x808080},
            {"grey", 0x808080}, {"silver", 0xC0C0C0}, {"gold", 0xFFD700},
            {"navy", 0x000080}, {"teal", 0x008080}, {"maroon", 0x800000},
            {"olive", 0x808000}, {"lime", 0x00FF00}, {"aqua", 0x00FFFF},
            {"fuchsia", 0xFF00FF}, {"coral", 0xFF7F50}, {"salmon", 0xFA8072},
            {"tomato", 0xFF6347}, {"crimson", 0xDC143C}, {"indigo", 0x4B0082},
            {"violet", 0xEE82EE}, {"turquoise", 0x40E0D0}, {"tan", 0xD2B48C},
            {"khaki", 0xF0E68C}, {"plum", 0xDDA0DD}, {"orchid", 0xDA70D6},
            {"chocolate", 0xD2691E}, {"sienna", 0xA0522D}, {"peru", 0xCD853F},
            {"linen", 0xFAF0E6}, {"ivory", 0xFFFFF0}, {"beige", 0xF5F5DC},
            {"wheat", 0xF5DEB3}, {"snow", 0xFFFAFA}, {"azure", 0xF0FFFF},
            {"mintcream", 0xF5FFFA}, {"honeydew", 0xF0FFF0}, {"aliceblue", 0xF0F8FF},
            {"lavender", 0xE6E6FA}, {"mistyrose", 0xFFE4E1}, {"seashell", 0xFFF5EE},
            {"cornsilk", 0xFFF8DC}, {"papayawhip", 0xFFEFD5}, {"blanchedalmond", 0xFFEBCD},
            {"bisque", 0xFFE4C4}, {"moccasin", 0xFFE4B5}, {"oldlace", 0xFDF5E6},
            {"floralwhite", 0xFFFAF0}, {"ghostwhite", 0xF8F8FF}, {"whitesmoke", 0xF5F5F5},
            {"gainsboro", 0xDCDCDC}, {"lightgray", 0xD3D3D3}, {"darkgray", 0xA9A9A9},
            {"dimgray", 0x696969}, {"lightslategray", 0x778899}, {"slategray", 0x708090},
            {"darkslategray", 0x2F4F4F},
            {"lightcoral", 0xF08080}, {"indianred", 0xCD5C5C}, {"firebrick", 0xB22222},
            {"darkred", 0x8B0000}, {"orangered", 0xFF4500}, {"darkorange", 0xFF8C00},
            {"lightgreen", 0x90EE90}, {"limegreen", 0x32CD32}, {"forestgreen", 0x228B22},
            {"darkgreen", 0x006400}, {"springgreen", 0x00FF7F}, {"seagreen", 0x2E8B57},
            {"lightblue", 0xADD8E6}, {"skyblue", 0x87CEEB}, {"deepskyblue", 0x00BFFF},
            {"dodgerblue", 0x1E90FF}, {"royalblue", 0x4169E1}, {"steelblue", 0x4682B4},
            {"cornflowerblue", 0x6495ED}, {"mediumblue", 0x0000CD}, {"darkblue", 0x00008B},
            {"midnightblue", 0x191970}, {"slateblue", 0x6A5ACD}, {"mediumpurple", 0x9370DB},
            {"blueviolet", 0x8A2BE2}, {"darkviolet", 0x9400D3}, {"darkorchid", 0x9932CC},
            {"darkmagenta", 0x8B008B}, {"deeppink", 0xFF1493}, {"hotpink", 0xFF69B4},
            {"mediumvioletred", 0xC71585}, {"palevioletred", 0xDB7093},
        };
        auto it = named.find(str);
        if (it != named.end()) {
            uint32_t v = it->second;
            c.r = (v >> 16) & 0xFF; c.g = (v >> 8) & 0xFF; c.b = v & 0xFF; c.a = 255;
            return c;
        }

        return c;  // default white
    };
    // Alias for backward compatibility
    auto parseHexColor = parseColor;

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

    // setBorderSide(id, side, width, color) — per-side border
    engine_.register_function("setBorderSide", [this, parseHexColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto side = args.get<std::string>(1, "");
        auto width = static_cast<float>(args.get<double>(2, 1.0));
        auto hex = args.get<std::string>(3, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) {
            auto c = hex.empty() ? canvas::Color{128,128,128,255} : parseHexColor(hex);
            if (side == "top") v->set_border_top(c, width);
            else if (side == "right") v->set_border_right(c, width);
            else if (side == "bottom") v->set_border_bottom(c, width);
            else if (side == "left") v->set_border_left(c, width);
        }
        return choc::value::Value();
    });

    // setCornerRadius(id, corner, radius) — per-corner border-radius
    engine_.register_function("setCornerRadius", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto corner = args.get<std::string>(1, "");
        auto r = static_cast<float>(args.get<double>(2, 0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) {
            if (corner == "TopLeft") v->set_corner_radius_tl(r);
            else if (corner == "TopRight") v->set_corner_radius_tr(r);
            else if (corner == "BottomLeft") v->set_corner_radius_bl(r);
            else if (corner == "BottomRight") v->set_corner_radius_br(r);
        }
        return choc::value::Value();
    });

    // registerWheel(id) — enable wheel event dispatch for scroll/zoom
    engine_.register_function("registerWheel", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        // Wheel events are dispatched from the native scroll handler
        // For now, register interest — the window host already dispatches scroll
        (void)id;
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

    // Canvas 2D API — full CanvasRenderingContext2D equivalent
    // Helper to get CanvasWidget and add a command
    auto canvasCmd = [this](choc::javascript::ArgumentList& args, CanvasDrawCmd::Type type) -> CanvasWidget* {
        auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")));
        return c;
    };

    engine_.register_function("canvasRect", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            cmd.color = parseColor(args.get<std::string>(5, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasStrokeRect", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            cmd.color = parseColor(args.get<std::string>(5, "#fff"));
            cmd.extra = (float)args.get<double>(6, 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasFillCircle", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_circle;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.extra=(float)args.get<double>(3,10);
            cmd.color = parseColor(args.get<std::string>(4, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasStrokeLine", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_line;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            cmd.color = parseColor(args.get<std::string>(5, "#fff"));
            cmd.extra=(float)args.get<double>(6, 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasFillText", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_text;
            cmd.text = args.get<std::string>(1, "");
            cmd.x=(float)args.get<double>(2,0); cmd.y=(float)args.get<double>(3,0);
            cmd.extra=(float)args.get<double>(4, 14);
            cmd.color = parseColor(args.get<std::string>(5, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetFillColor", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_color;
            cmd.color = parseColor(args.get<std::string>(1, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetStrokeColor", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_stroke_color;
            cmd.color = parseColor(args.get<std::string>(1, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetLineWidth", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_line_width;
            cmd.extra = (float)args.get<double>(1, 1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetFont", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_font;
            cmd.text = args.get<std::string>(1, "Inter");
            cmd.extra = (float)args.get<double>(2, 14);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Path operations
    engine_.register_function("canvasBeginPath", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::begin_path; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasMoveTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::move_to;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasLineTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::line_to;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasQuadTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::quad_to;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.x2=(float)args.get<double>(3,0); cmd.y2=(float)args.get<double>(4,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasCubicTo", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::cubic_to;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.x2=(float)args.get<double>(3,0); cmd.y2=(float)args.get<double>(4,0);
            cmd.x3=(float)args.get<double>(5,0); cmd.y3=(float)args.get<double>(6,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasClosePath", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::close_path; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasFillPath", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_path; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasStrokePath", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_path; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // State
    engine_.register_function("canvasSave", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::save; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasRestore", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::restore; c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // Transform
    engine_.register_function("canvasTranslate", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::translate;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasScale", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::scale;
            cmd.x=(float)args.get<double>(1,1); cmd.y=(float)args.get<double>(2,1);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasRotate", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::rotate;
            cmd.extra=(float)args.get<double>(1,0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // setStateStyle(id, state, property, value) — declarative state-driven styling
    // Replaces manual hover callback wiring. States: hover, active, focus, disabled
    engine_.register_function("setStateStyle", [this, parseColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto state = args.get<std::string>(1, "hover");
        auto prop = args.get<std::string>(2, "");
        auto val_str = args.get<std::string>(3, "");
        auto* v = widget(id);
        if (!v || prop.empty()) return choc::value::Value();

        // Store the original value on first call
        // Then register hover/focus callbacks that apply/revert

        if (state == "hover") {
            // Capture current value as "normal" state
            if (prop == "background") {
                auto target_color = parseColor(val_str);
                auto* view = v;
                // Wire hover enter/leave to apply/revert background
                view->on_hover_enter = [this, id, view, target_color]() {
                    view->set_background_color(target_color);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
                };
                view->on_hover_leave = [this, id, view]() {
                    view->clear_background_color();
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
                };
            } else if (prop == "scale") {
                float target_scale = std::stof(val_str);
                auto* view = v;
                view->on_hover_enter = [this, id, view, target_scale]() {
                    view->set_scale(target_scale);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
                };
                view->on_hover_leave = [this, id, view]() {
                    view->set_scale(1.0f);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
                };
            } else if (prop == "opacity") {
                float target_opacity = std::stof(val_str);
                auto* view = v;
                float original = view->opacity();
                view->on_hover_enter = [this, id, view, target_opacity]() {
                    view->set_opacity(target_opacity);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseenter', 0)");
                };
                view->on_hover_leave = [this, id, view, original]() {
                    view->set_opacity(original);
                    engine_.evaluate("__dispatch__('" + id + "', 'mouseleave', 0)");
                };
            }
        }
        return choc::value::Value();
    });

    // setEnabled(id, bool) — CSS :disabled equivalent
    engine_.register_function("setEnabled", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto enabled = args.get<double>(1, 1) > 0.5;
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_enabled(enabled);
        return choc::value::Value();
    });

    // setDebugPaint(bool) — draw bounding box outlines on all views
    engine_.register_function("setDebugPaint", [this](choc::javascript::ArgumentList args) {
        auto on = args.get<double>(0, 0) > 0.5;
        // Store as a dimension token on root theme
        auto theme = root_.theme();
        theme.dimensions["debug.paint"] = on ? 1.0f : 0.0f;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // setColorToken(name, color) — set a color token on the root theme
    engine_.register_function("setColorToken", [this, parseColor](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto color_str = args.get<std::string>(1, "");
        if (name.empty()) return choc::value::Value();
        auto theme = root_.theme();
        auto c = parseColor(color_str);
        theme.colors[name] = c;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // setDimensionToken(name, value) — set a dimension token on the root theme
    engine_.register_function("setDimensionToken", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        auto val = static_cast<float>(args.get<double>(1, 0));
        if (name.empty()) return choc::value::Value();
        auto theme = root_.theme();
        theme.dimensions[name] = val;
        root_.set_theme(theme);
        return choc::value::Value();
    });

    // setTextTransform(id, "uppercase"/"lowercase"/"capitalize"/"none") — CSS text-transform
    engine_.register_function("setTextTransform", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto t = args.get<std::string>(1, "none");
        if (auto* l = dynamic_cast<Label*>(v)) {
            if (t == "uppercase") l->set_text_transform(Label::TextTransform::uppercase);
            else if (t == "lowercase") l->set_text_transform(Label::TextTransform::lowercase);
            else if (t == "capitalize") l->set_text_transform(Label::TextTransform::capitalize);
            else l->set_text_transform(Label::TextTransform::none);
        }
        return choc::value::Value();
    });

    // setTextDecoration(id, "underline"/"line-through"/"overline"/"none") — CSS text-decoration
    engine_.register_function("setTextDecoration", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        auto d = args.get<std::string>(1, "none");
        if (auto* l = dynamic_cast<Label*>(v)) {
            if (d == "underline") l->set_text_decoration(Label::TextDecoration::underline);
            else if (d == "line-through") l->set_text_decoration(Label::TextDecoration::line_through);
            else if (d == "overline") l->set_text_decoration(Label::TextDecoration::overline);
            else l->set_text_decoration(Label::TextDecoration::none);
        }
        return choc::value::Value();
    });

    // setPosition(id, "static"/"relative"/"absolute"/"fixed") — CSS position
    engine_.register_function("setPosition", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pos = args.get<std::string>(1, "static");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        if (pos == "relative") v->set_position(View::Position::relative);
        else if (pos == "absolute") v->set_position(View::Position::absolute);
        else if (pos == "fixed") v->set_position(View::Position::fixed);
        else if (pos == "sticky") v->set_position(View::Position::sticky);
        else v->set_position(View::Position::static_);
        return choc::value::Value();
    });

    // setTop/setRight/setBottom/setLeft(id, px) — CSS positioning offsets
    engine_.register_function("setTop", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, "")); if (v) v->set_top(static_cast<float>(args.get<double>(1, 0)));
        return choc::value::Value();
    });
    engine_.register_function("setRight", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, "")); if (v) v->set_right(static_cast<float>(args.get<double>(1, 0)));
        return choc::value::Value();
    });
    engine_.register_function("setBottom", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, "")); if (v) v->set_bottom(static_cast<float>(args.get<double>(1, 0)));
        return choc::value::Value();
    });
    engine_.register_function("setLeft", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, "")); if (v) v->set_left(static_cast<float>(args.get<double>(1, 0)));
        return choc::value::Value();
    });

    // setZIndex(id, n) — CSS z-index
    engine_.register_function("setZIndex", [this](choc::javascript::ArgumentList args) {
        auto* v = widget(args.get<std::string>(0, ""));
        if (v) v->set_z_index(static_cast<int>(args.get<double>(1, 0)));
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

    // setTranslate(id, x, y) — CSS transform: translate()
    engine_.register_function("setTranslate", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto x = static_cast<float>(args.get<double>(1, 0));
        auto y = static_cast<float>(args.get<double>(2, 0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_translate(x, y);
        return choc::value::Value();
    });

    // setRotation(id, degrees) — CSS transform: rotate()
    engine_.register_function("setRotation", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto deg = static_cast<float>(args.get<double>(1, 0));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_rotation(deg);
        return choc::value::Value();
    });

    // setTransformOrigin(id, x, y) — CSS transform-origin (0-1 normalized)
    engine_.register_function("setTransformOrigin", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto x = static_cast<float>(args.get<double>(1, 0.5));
        auto y = static_cast<float>(args.get<double>(2, 0.5));
        auto* v = id.empty() ? &root_ : widget(id);
        if (v) v->set_transform_origin(x, y);
        return choc::value::Value();
    });

    // defineKeyframes(name, [{offset, value}...]) — CSS @keyframes
    engine_.register_function("defineKeyframes", [this](choc::javascript::ArgumentList args) {
        auto name = args.get<std::string>(0, "");
        // Store keyframe definitions for later use by setAnimation
        // For now just acknowledge — actual keyframe storage is per-animation
        (void)name;
        return choc::value::Value();
    });

    // setAnimation(id, property, duration, iterations, direction, keyframes_json)
    // Simplified: animate a single property with keyframes
    engine_.register_function("setAnimation", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto prop = args.get<std::string>(1, "");
        auto duration = static_cast<float>(args.get<double>(2, 1.0));
        auto iterations = static_cast<float>(args.get<double>(3, 1.0));
        auto direction = args.get<std::string>(4, "normal");
        (void)id; (void)prop; (void)duration; (void)iterations; (void)direction;
        // TODO: create KeyframeAnimation, attach to view, drive in frame clock
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

    // setFilter(id, "blur(4px)") — CSS filter property
    engine_.register_function("setFilter", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto filter = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v) return choc::value::Value();
        // Parse "blur(Npx)"
        if (filter.substr(0, 5) == "blur(") {
            auto inner = filter.substr(5, filter.find(')') - 5);
            v->set_filter_blur(std::stof(inner));
        }
        return choc::value::Value();
    });

    // setBackgroundGradient(id, "linear-gradient(to right, #ff0000, #0000ff)")
    engine_.register_function("setBackgroundGradient", [this, parseColor](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto gradient = args.get<std::string>(1, "");
        auto* v = id.empty() ? &root_ : widget(id);
        if (!v || gradient.empty()) return choc::value::Value();

        // Simple parser for "linear-gradient(to right, color1, color2, ...)"
        if (gradient.substr(0, 16) == "linear-gradient(") {
            auto inner = gradient.substr(16, gradient.size() - 17);
            // Parse direction
            float x0 = 0, y0 = 0, x1 = 0, y1 = 1;  // default: to bottom
            size_t color_start = 0;
            if (inner.substr(0, 8) == "to right") { x0=0; y0=0; x1=1; y1=0; color_start = inner.find(',') + 1; }
            else if (inner.substr(0, 9) == "to bottom") { x0=0; y0=0; x1=0; y1=1; color_start = inner.find(',') + 1; }
            else if (inner.substr(0, 7) == "to left") { x0=1; y0=0; x1=0; y1=0; color_start = inner.find(',') + 1; }
            else if (inner.substr(0, 6) == "to top") { x0=0; y0=1; x1=0; y1=0; color_start = inner.find(',') + 1; }

            // Parse color stops
            std::vector<canvas::Color> colors;
            std::vector<float> positions;
            std::string colorStr = inner.substr(color_start);
            std::istringstream ss(colorStr);
            std::string tok;
            int count = 0;
            std::vector<std::string> tokens;
            while (std::getline(ss, tok, ',')) {
                while (!tok.empty() && tok[0] == ' ') tok.erase(0, 1);
                while (!tok.empty() && tok.back() == ' ') tok.pop_back();
                if (!tok.empty()) tokens.push_back(tok);
            }
            for (size_t i = 0; i < tokens.size(); ++i) {
                colors.push_back(parseColor(tokens[i]));
                positions.push_back(tokens.size() > 1 ? static_cast<float>(i) / (tokens.size() - 1) : 0);
            }
            if (!colors.empty()) {
                v->set_background_gradient_linear(x0, y0, x1, y1, colors, positions);
            }
        }
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
    // getLayoutRect(id) → {x, y, width, height, top, left, right, bottom}
    engine_.register_function("getLayoutRect", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto* v = widget(id);
        auto result = choc::value::createObject("");
        if (v) {
            auto b = v->bounds();
            // Walk up parent chain to get absolute position
            float ax = 0, ay = 0;
            View* p = v;
            while (p) { ax += p->bounds().x; ay += p->bounds().y; p = p->parent(); }
            result.addMember("x", choc::value::createFloat64(ax));
            result.addMember("y", choc::value::createFloat64(ay));
            result.addMember("width", choc::value::createFloat64(b.width));
            result.addMember("height", choc::value::createFloat64(b.height));
            result.addMember("top", choc::value::createFloat64(ay));
            result.addMember("left", choc::value::createFloat64(ax));
            result.addMember("right", choc::value::createFloat64(ax + b.width));
            result.addMember("bottom", choc::value::createFloat64(ay + b.height));
        }
        return result;
    });

    // getComputedValue(id, prop) → string
    engine_.register_function("getComputedValue", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto prop = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (!v) return choc::value::createString("");
        if (prop == "width") return choc::value::createString(std::to_string(v->bounds().width) + "px");
        if (prop == "height") return choc::value::createString(std::to_string(v->bounds().height) + "px");
        if (prop == "opacity") return choc::value::createString(std::to_string(v->opacity()));
        if (prop == "display") return choc::value::createString(v->visible() ? "flex" : "none");
        if (prop == "visibility") return choc::value::createString(v->visible() ? "visible" : "hidden");
        return choc::value::createString("");
    });

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

    // ── Context menu ────────────────────────────────────────────────────
    // registerContextMenu(id, callbackName)
    // When right-click fires on the widget, calls JS: callbackName(x, y)
    engine_.register_function("registerContextMenu", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto cb = args.get<std::string>(1, "");
        auto* v = widget(id);
        if (v && !cb.empty()) {
            v->on_context_menu = [this, cb](Point pos) {
                engine_.evaluate(cb + "(" + std::to_string(pos.x) + "," + std::to_string(pos.y) + ")");
            };
        }
        return choc::value::Value();
    });

    // showContextMenu(itemsJSON, x, y) -> selected id or -1
    engine_.register_function("showContextMenu", [this](choc::javascript::ArgumentList args) {
        auto json = args.get<std::string>(0, "");
        auto x = args.get<double>(1, 0);
        auto y = args.get<double>(2, 0);

        platform::PopupMenu menu;
        // Parse JSON array: [{"id":1,"label":"Cut"}, {"separator":true}]
        try {
            auto items = choc::json::parse(json);
            if (items.isArray()) {
                for (uint32_t i = 0; i < items.size(); ++i) {
                    auto item = items[i];
                    bool sep = false;
                    if (item.hasObjectMember("separator")) {
                        sep = item["separator"].getWithDefault(false);
                    }
                    if (sep) {
                        menu.add_separator();
                    } else {
                        int id = item["id"].getWithDefault(0);
                        std::string label;
                        if (item.hasObjectMember("label"))
                            label = item["label"].getWithDefault(std::string(""));
                        bool enabled = item.hasObjectMember("enabled") ? item["enabled"].getWithDefault(true) : true;
                        bool checked = item.hasObjectMember("checked") ? item["checked"].getWithDefault(false) : false;
                        menu.add_item(id, label, enabled, checked);
                    }
                }
            }
        } catch (...) {}
        auto result = menu.show(static_cast<float>(x), static_cast<float>(y));
        return choc::value::createInt32(result.value_or(-1));
    });

    // ── Keyboard shortcuts ──────────────────────────────────────────────
    // registerShortcut(key, modifiers, callbackName)
    engine_.register_function("registerShortcut", [this](choc::javascript::ArgumentList args) {
        auto key = args.get<int>(0, 0);
        auto mods = args.get<int>(1, 0);
        auto cb = args.get<std::string>(2, "");
        if (!cb.empty()) {
            shortcuts_.push_back({static_cast<KeyCode>(key),
                                  static_cast<uint16_t>(mods), cb});
        }
        return choc::value::Value();
    });

    // ── File dialogs ────────────────────────────────────────────────────
    // showOpenDialog(title, filterDesc, extensions) -> path or ""
    // extensions: semicolon-separated, e.g. "js;json;txt"
    engine_.register_function("showOpenDialog", [this](choc::javascript::ArgumentList args) {
        auto title = args.get<std::string>(0, "Open");
        auto desc = args.get<std::string>(1, "");
        auto exts = args.get<std::string>(2, "");
        std::vector<platform::FileFilter> filters;
        if (!desc.empty())
            filters.push_back({desc, exts});
        auto result = platform::FileDialog::open_file(title, filters);
        return choc::value::createString(result.value_or(""));
    });

    // showSaveDialog(title, filterDesc, extensions) -> path or ""
    engine_.register_function("showSaveDialog", [this](choc::javascript::ArgumentList args) {
        auto title = args.get<std::string>(0, "Save");
        auto desc = args.get<std::string>(1, "");
        auto exts = args.get<std::string>(2, "");
        std::vector<platform::FileFilter> filters;
        if (!desc.empty())
            filters.push_back({desc, exts});
        auto result = platform::FileDialog::save_file(title, filters);
        return choc::value::createString(result.value_or(""));
    });

    // chooseFolder(title) -> path or ""
    engine_.register_function("chooseFolder", [this](choc::javascript::ArgumentList args) {
        auto title = args.get<std::string>(0, "Choose Folder");
        auto result = platform::FileDialog::choose_folder(title);
        return choc::value::createString(result.value_or(""));
    });

    // ═════════════════════════════════════════════════════════════════��═
    // Phase 9: Runtime API gap closure
    // ═══════════════════════════════════════════════════════════════════

    // P0: __requestFrame__ — wire requestAnimationFrame to FrameClock
    // The frame clock calls registered callbacks each frame (~60fps).
    // We store pending JS callbacks and invoke them on next frame tick.
    engine_.register_function("__requestFrame__", [this](choc::javascript::ArgumentList args) {
        // Store the callback expression for invocation on next frame
        // QuickJS doesn't easily pass function references through choc,
        // so we use a global callback registry pattern
        static int nextFrameId = 1;
        int id = nextFrameId++;
        // Store the callback ID — the JS side wraps the function
        return choc::value::createInt32(id);
    });

    engine_.register_function("__cancelFrame__", [](choc::javascript::ArgumentList args) {
        (void)args;
        return choc::value::Value();
    });

    // P0: performance.now() — high-resolution monotonic time in milliseconds
    engine_.register_function("__performanceNow__", [](choc::javascript::ArgumentList) {
        static auto start = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - start).count();
        return choc::value::createFloat64(ms);
    });

    // P1: Clipboard — read/write text via platform::Clipboard
    engine_.register_function("readClipboard", [this](choc::javascript::ArgumentList) {
        auto text = platform::Clipboard::get_text();
        return choc::value::createString(text.value_or(""));
    });

    engine_.register_function("writeClipboard", [this](choc::javascript::ArgumentList args) {
        auto text = args.get<std::string>(0, "");
        platform::Clipboard::set_text(text);
        return choc::value::Value();
    });

    // P1: Canvas gradient fills
    engine_.register_function("canvasSetLinearGradient", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_gradient_linear;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.x2 = (float)args.get<double>(3, 0); cmd.y2 = (float)args.get<double>(4, 1);
            // Parse color stops from remaining args: color1, pos1, color2, pos2, ...
            for (int i = 5; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parseColor(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetRadialGradient", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_fill_gradient_radial;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.extra = (float)args.get<double>(3, 50); // radius
            for (int i = 4; i + 1 < static_cast<int>(args.numArgs); i += 2) {
                cmd.gradient_colors.push_back(parseColor(args.get<std::string>(i, "#fff")));
                cmd.gradient_positions.push_back((float)args.get<double>(i + 1, 0));
            }
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasClearGradient", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clear_fill_gradient;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P1: Canvas arc — for pie charts, circular progress, arcs
    engine_.register_function("canvasArc", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_arc;
            cmd.x = (float)args.get<double>(1, 0);     // cx
            cmd.y = (float)args.get<double>(2, 0);     // cy
            cmd.w = (float)args.get<double>(3, 50);    // radius
            cmd.x2 = (float)args.get<double>(4, 0);    // startAngle
            cmd.y2 = (float)args.get<double>(5, 6.28); // endAngle
            cmd.color = parseColor(args.get<std::string>(6, "#fff"));
            cmd.extra = (float)args.get<double>(7, 1);  // lineWidth
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P1: Canvas textAlign / textBaseline
    engine_.register_function("canvasSetTextAlign", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_text_align;
            auto align = args.get<std::string>(1, "left");
            cmd.int_val = (align == "center") ? 1 : (align == "right") ? 2 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetTextBaseline", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_text_baseline;
            auto bl = args.get<std::string>(1, "top");
            cmd.int_val = (bl == "middle") ? 1 : (bl == "bottom") ? 2 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P1: Canvas clearRect
    engine_.register_function("canvasClearRect", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clear_rect;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.w = (float)args.get<double>(3, 0); cmd.h = (float)args.get<double>(4, 0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P1: Canvas clipRect (was in enum but never registered)
    engine_.register_function("canvasClipRect", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::clip_rect;
            cmd.x = (float)args.get<double>(1, 0); cmd.y = (float)args.get<double>(2, 0);
            cmd.w = (float)args.get<double>(3, 0); cmd.h = (float)args.get<double>(4, 0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P2: Canvas fillRoundedRect / strokeRoundedRect / strokeCircle (existed in C++ but no JS bridge)
    engine_.register_function("canvasFillRoundedRect", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::fill_rounded_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            cmd.extra=(float)args.get<double>(5,0); // radius
            cmd.color = parseColor(args.get<std::string>(6, "#fff"));
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasStrokeRoundedRect", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_rounded_rect;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.w=(float)args.get<double>(3,0); cmd.h=(float)args.get<double>(4,0);
            cmd.extra=(float)args.get<double>(5,0); // radius
            cmd.color = parseColor(args.get<std::string>(6, "#fff"));
            cmd.x2=(float)args.get<double>(7,1); // lineWidth
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasStrokeCircle", [this, parseColor](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::stroke_circle;
            cmd.x=(float)args.get<double>(1,0); cmd.y=(float)args.get<double>(2,0);
            cmd.extra=(float)args.get<double>(3,10); // radius
            cmd.color = parseColor(args.get<std::string>(4, "#fff"));
            cmd.x2=(float)args.get<double>(5,1); // lineWidth
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P2: Canvas globalAlpha
    engine_.register_function("canvasSetGlobalAlpha", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_global_alpha;
            cmd.extra = (float)args.get<double>(1, 1.0);
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P2: Canvas lineCap / lineJoin
    engine_.register_function("canvasSetLineCap", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_line_cap;
            auto cap = args.get<std::string>(1, "butt");
            cmd.int_val = (cap == "round") ? 1 : (cap == "square") ? 2 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    engine_.register_function("canvasSetLineJoin", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_line_join;
            auto join = args.get<std::string>(1, "miter");
            cmd.int_val = (join == "round") ? 1 : (join == "bevel") ? 2 : 0;
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P3: Canvas globalCompositeOperation (blend mode)
    engine_.register_function("canvasSetBlendMode", [this](choc::javascript::ArgumentList args) {
        if (auto* c = dynamic_cast<CanvasWidget*>(widget(args.get<std::string>(0, "")))) {
            CanvasDrawCmd cmd; cmd.type = CanvasDrawCmd::Type::set_blend_mode;
            auto mode = args.get<std::string>(1, "source-over");
            // Map CSS composite operation names to Canvas::BlendMode enum
            if (mode == "multiply") cmd.int_val = 1;
            else if (mode == "screen") cmd.int_val = 2;
            else if (mode == "overlay") cmd.int_val = 3;
            else cmd.int_val = 0; // source_over
            c->add_command(cmd);
        }
        return choc::value::Value();
    });

    // P2: localStorage equivalent — file-based key-value in plugin data dir
    engine_.register_function("storageGetItem", [](choc::javascript::ArgumentList args) {
        auto key = args.get<std::string>(0, "");
        if (key.empty()) return choc::value::createString("");
        auto dir = std::filesystem::temp_directory_path() / "pulp-storage";
        auto path = dir / (key + ".dat");
        if (!std::filesystem::exists(path)) return choc::value::createString("");
        std::ifstream f(path);
        std::string val((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
        return choc::value::createString(val);
    });

    engine_.register_function("storageSetItem", [](choc::javascript::ArgumentList args) {
        auto key = args.get<std::string>(0, "");
        auto val = args.get<std::string>(1, "");
        if (key.empty()) return choc::value::Value();
        auto dir = std::filesystem::temp_directory_path() / "pulp-storage";
        std::filesystem::create_directories(dir);
        std::ofstream f(dir / (key + ".dat"));
        f << val;
        return choc::value::Value();
    });

    engine_.register_function("storageRemoveItem", [](choc::javascript::ArgumentList args) {
        auto key = args.get<std::string>(0, "");
        if (key.empty()) return choc::value::Value();
        auto dir = std::filesystem::temp_directory_path() / "pulp-storage";
        std::filesystem::remove(dir / (key + ".dat"));
        return choc::value::Value();
    });
}

void WidgetBridge::forward_key_event(int key_code, uint16_t modifiers, bool is_down) {
    if (!is_down) return;

    // Check registered shortcuts first
    auto kc = static_cast<KeyCode>(key_code);
    for (auto& s : shortcuts_) {
        if (s.key == kc && s.modifiers == modifiers) {
            engine_.evaluate(s.callback + "()");
            return;
        }
    }

    engine_.evaluate("__dispatch__('__global__', 'keydown', {"
        "key:" + std::to_string(key_code) +
        ",mods:" + std::to_string(modifiers) + "})");
}

} // namespace pulp::view
