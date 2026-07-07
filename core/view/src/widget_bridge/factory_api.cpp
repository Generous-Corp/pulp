#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/native_view_host.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/virtual_list.hpp>

#include <atomic>
#include <iostream>
#include <memory>
#include <string>

namespace pulp::view {

namespace {

void safe_dispatch_eval(const std::shared_ptr<std::atomic<bool>>& alive,
                        ScriptEngine* engine,
                        const std::string& js,
                        const char* context) {
    if (!alive || !alive->load(std::memory_order_acquire) || engine == nullptr) return;
    try {
        if (!static_cast<bool>(*engine)) return;
        engine->evaluate(js);
        engine->pump_message_loop();
    } catch (const std::exception& e) {
        std::cerr << "WidgetBridge " << context << " error: " << e.what() << "\n";
    } catch (...) {
        std::cerr << "WidgetBridge " << context << " error: unknown exception\n";
    }
}

bool is_new_widget_factory_api(choc::javascript::ArgumentList& args) {
    if (args.numArgs <= 2) return true;
    const auto* second = args[1];
    if (second == nullptr) return true;
    if (second->isInt32() || second->isInt64() ||
        second->isFloat32() || second->isFloat64()) {
        return false;
    }
    if (!second->isString()) return true;
    const auto test = second->getWithDefault<std::string>("");
    if (test.empty()) return true;
    if (test[0] >= '0' && test[0] <= '9') return false;
    if (test[0] == '-') return false;
    return true;
}

} // namespace

void WidgetBridge::register_widget_factory_controls_api() {
    auto isNewApi = is_new_widget_factory_api;
    BridgeApiContext api{engine_};

    // createKnob(id, parentId) OR createKnob(id, x, y, w, h)
    register_bridge_function(api, "createKnob", [this, isNewApi](choc::javascript::ArgumentList args) {
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
            auto* ptr = knob.get();
            widgets_[id] = ptr;
            wire_callbacks(id, ptr);
            root_.add_child(std::move(knob));
        }
        return choc::value::createString(id);
    });

    // createFader(id, orientation, parentId) OR createFader(id, x, y, w, h, orientation)
    register_bridge_function(api, "createFader", [this, isNewApi](choc::javascript::ArgumentList args) {
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
            auto* ptr = fader.get();
            widgets_[id] = ptr;
            wire_callbacks(id, ptr);
            root_.add_child(std::move(fader));
        }
        return choc::value::createString(id);
    });

    // createToggle(id, parentId) OR createToggle(id, x, y, w, h)
    register_bridge_function(api, "createToggle", [this, isNewApi](choc::javascript::ArgumentList args) {
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
            auto* ptr = toggle.get();
            widgets_[id] = ptr;
            wire_callbacks(id, ptr);
            root_.add_child(std::move(toggle));
        }
        return choc::value::createString(id);
    });

    // createRangeSlider(id, parentId)
    register_bridge_function(api, "createRangeSlider", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto rs = std::make_unique<RangeSlider>();
        rs->set_id(id);
        auto* ptr = rs.get();
        widgets_[id] = ptr;
        wire_callbacks(id, ptr);
        resolve_parent(pid)->add_child(std::move(rs));
        return choc::value::createString(id);
    });

    // createIcon(id, type, parentId) - type: "image_upload", "send", "search", "close"
    register_bridge_function(api, "createIcon", [this](choc::javascript::ArgumentList args) {
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

    // createImage(id, parentId) - HTML <img> equivalent
    register_bridge_function(api, "createImage", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto img = std::make_unique<ImageView>(); img->set_id(id);
        widgets_[id] = img.get();
        resolve_parent(pid)->add_child(std::move(img));
        return choc::value::createString(id);
    });
}

void WidgetBridge::register_widget_factory_form_api() {
    BridgeApiContext api{engine_};

    // createCheckbox(id, parentId)
    register_bridge_function(api, "createCheckbox", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto cb = std::make_unique<Checkbox>(); cb->set_id(id);
        auto* ptr = cb.get(); widgets_[id] = ptr;
        auto alive = callback_alive_;
        auto* engine = &engine_;
        cb->on_change = [alive, engine, id](bool v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::string(v?"1":"0") + ")", "checkbox change");
        };
        resolve_parent(pid)->add_child(std::move(cb));
        return choc::value::createString(id);
    });

    // createToggleButton(id, parentId)
    register_bridge_function(api, "createToggleButton", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto tb = std::make_unique<ToggleButton>(); tb->set_id(id);
        auto* ptr = tb.get(); widgets_[id] = ptr;
        auto alive = callback_alive_;
        auto* engine = &engine_;
        tb->on_toggle = [alive, engine, id](bool v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'toggle', " + std::string(v?"1":"0") + ")", "toggle button");
        };
        resolve_parent(pid)->add_child(std::move(tb));
        return choc::value::createString(id);
    });

    // createLabel(id, text, parentId) OR createLabel(id, text, x, y, w, h)
    register_bridge_function(api, "createLabel", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto text = args.get<std::string>(1, "");

        // For Label: old API is (id, text, x, y, w, h) - arg[2] is number
        // New API is (id, text, parentId) - arg[2] is string or absent
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
}

void WidgetBridge::register_widget_factory_container_api() {
    BridgeApiContext api{engine_};

    register_bridge_function(api, "createRow", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<View>(); v->set_id(id);
        v->flex().direction = FlexDirection::row;
        widgets_[id] = v.get();
        resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createCol", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<View>(); v->set_id(id);
        v->flex().direction = FlexDirection::column;
        widgets_[id] = v.get();
        resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createModal", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<ModalOverlay>(); v->set_id(id);
        v->flex().direction = FlexDirection::column;
        auto* modal = v.get();
        auto alive = callback_alive_;
        auto* engine = &engine_;
        modal->on_dismiss = [alive, engine, modal, id]() {
            if (modal) {
                modal->set_visible(false);
            }
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'dismiss', 0)", "modal dismiss");
        };
        widgets_[id] = v.get();
        resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createPanel", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<Panel>(); p->set_id(id);
        widgets_[id] = p.get();
        resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });
}

void WidgetBridge::register_widget_factory_composite_api() {
    BridgeApiContext api{engine_};

    register_bridge_function(api, "createMeter", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto o = args.get<std::string>(1, "vertical");
        auto pid = args.get<std::string>(2, "");
        auto m = std::make_unique<Meter>(); m->set_id(id);
        if (o == "horizontal") m->set_orientation(Meter::Orientation::horizontal);
        widgets_[id] = m.get(); resolve_parent(pid)->add_child(std::move(m));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createXYPad", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<XYPad>(); p->set_id(id);
        widgets_[id] = p.get(); resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createWaveform", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto w = std::make_unique<WaveformView>(); w->set_id(id);
        widgets_[id] = w.get(); resolve_parent(pid)->add_child(std::move(w));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createSpectrum", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto s = std::make_unique<SpectrumView>(); s->set_id(id);
        widgets_[id] = s.get(); resolve_parent(pid)->add_child(std::move(s));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createCombo", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto c = std::make_unique<ComboBox>(); c->set_id(id);
        auto* ptr = c.get(); widgets_[id] = ptr;
        auto alive = callback_alive_;
        auto* engine = &engine_;
        c->on_change = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'select', " + std::to_string(idx) + ")", "combo select");
        };
        resolve_parent(pid)->add_child(std::move(c));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createProgress", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<ProgressBar>(); p->set_id(id);
        widgets_[id] = p.get(); resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createScrollView", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto s = std::make_unique<ScrollView>(); s->set_id(id);
        widgets_[id] = s.get(); resolve_parent(pid)->add_child(std::move(s));
        return choc::value::createString(id);
    });

    // A layout box for a platform-native child view (WebView / native text
    // field / video layer). Materializes empty — JS can't mint an OS view
    // handle — so a C++ host binds the native child by id afterwards via
    // `dynamic_cast<NativeViewHost*>(bridge.widget(id))->set_native_child(...)`.
    register_bridge_function(api, "createNativeView", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto nv = std::make_unique<NativeViewHost>(); nv->set_id(id);
        widgets_[id] = nv.get(); resolve_parent(pid)->add_child(std::move(nv));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createListBox", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto lb = std::make_unique<ListBox>(); lb->set_id(id);
        auto* ptr = lb.get(); widgets_[id] = ptr;
        auto alive = callback_alive_;
        auto* engine = &engine_;
        lb->on_select = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'select', " + std::to_string(idx) + ")", "list select");
        };
        lb->on_activate = [alive, engine, id](int idx) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'activate', " + std::to_string(idx) + ")", "list activate");
        };
        resolve_parent(pid)->add_child(std::move(lb));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createVirtualList", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto list = std::make_unique<VirtualList>(); list->set_id(id);
        auto* ptr = list.get(); widgets_[id] = ptr;
        wire_callbacks(id, ptr);
        resolve_parent(pid)->add_child(std::move(list));
        return choc::value::createString(id);
    });

}

void WidgetBridge::register_widget_factory_text_editor_api() {
    BridgeApiContext api{engine_};

    register_bridge_function(api, "createTextEditor", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto ed = std::make_unique<TextEditor>(); ed->set_id(id);
        auto* ptr = ed.get(); widgets_[id] = ptr;
        auto alive = callback_alive_;
        auto* engine = &engine_;
        ed->on_escape = [alive, engine, id]() {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'escape', 0)", "text escape");
        };
        ed->on_return = [alive, engine, id](const std::string& text) {
            std::string e; for (char c : text) { if (c=='\'') e+= "\\'"; else if (c=='\n') e+= "\\n"; else e+= c; }
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'return', '" + e + "')", "text return");
        };
        ed->on_change = [alive, engine, id](const std::string& text) {
            std::string e; for (char c : text) { if (c=='\'') e+= "\\'"; else if (c=='\n') e+= "\\n"; else e+= c; }
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', '" + e + "')", "text change");
        };
        resolve_parent(pid)->add_child(std::move(ed));
        return choc::value::createString(id);
    });
}

void WidgetBridge::register_widget_factory_design_system_api() {
    BridgeApiContext api{engine_};
    auto tone_from = [](const std::string& s) {
        if (s == "info") return Tone::info;
        if (s == "success") return Tone::success;
        if (s == "warning") return Tone::warning;
        if (s == "danger") return Tone::danger;
        return Tone::neutral;
    };

    // createBadge(id, text, tone, parentId) — Ink & Signal status pill.
    register_bridge_function(api, "createBadge", [this, tone_from](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto text = args.get<std::string>(1, "");
        auto tone = args.get<std::string>(2, "neutral");
        auto pid = args.get<std::string>(3, "");
        auto b = std::make_unique<Badge>(text, tone_from(tone));
        b->set_id(id);
        widgets_[id] = b.get();
        resolve_parent(pid)->add_child(std::move(b));
        return choc::value::createString(id);
    });

    // createStepper(id, parentId) — [-] value [+] numeric nudge.
    register_bridge_function(api, "createStepper", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto s = std::make_unique<Stepper>();
        s->set_id(id);
        auto* ptr = s.get();
        widgets_[id] = ptr;
        auto alive = callback_alive_;
        auto* engine = &engine_;
        s->on_change = [alive, engine, id](double v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::to_string(v) + ")", "stepper change");
        };
        resolve_parent(pid)->add_child(std::move(s));
        return choc::value::createString(id);
    });

    // createPan(id, parentId) — bipolar 1-D pan (-1..+1).
    register_bridge_function(api, "createPan", [this](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<PanControl>();
        p->set_id(id);
        auto* ptr = p.get();
        widgets_[id] = ptr;
        auto alive = callback_alive_;
        auto* engine = &engine_;
        p->on_change = [alive, engine, id](float v) {
            safe_dispatch_eval(alive, engine, "__dispatch__('" + id + "', 'change', " + std::to_string(v) + ")", "pan change");
        };
        resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });
}

} // namespace pulp::view
