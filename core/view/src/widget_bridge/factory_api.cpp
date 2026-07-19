#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"
#include "bridge_dispatch.hpp"

#include <pulp/view/gap_widgets.hpp>
#include <pulp/view/native_view_host.hpp>
#include <pulp/view/modal.hpp>
#include <pulp/view/text_editor.hpp>
#include <pulp/view/ui_components.hpp>
#include <pulp/view/virtual_list.hpp>
#include <pulp/view/virtual_grid.hpp>

#include <atomic>
#include <memory>
#include <string>

namespace pulp::view {

namespace {

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

void BridgeRegistrars::register_widget_factory_controls_api(WidgetBridge& self) {
    auto isNewApi = is_new_widget_factory_api;
    BridgeApiContext api{self.engine_};

    // createKnob(id, parentId) OR createKnob(id, x, y, w, h)
    register_bridge_function(api, "createKnob", [&self, isNewApi](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto knob = std::make_unique<Knob>();
        knob->set_id(id);
        knob->set_label(id);

        if (isNewApi(args)) {
            auto pid = args.get<std::string>(1, "");
            auto* ptr = knob.get();
            self.widgets_[id] = ptr;
            self.wire_callbacks(id, ptr);
            self.resolve_parent(pid)->add_child(std::move(knob));
        } else {
            knob->set_bounds({(float)args.get<double>(1,0), (float)args.get<double>(2,0),
                             (float)args.get<double>(3,48), (float)args.get<double>(4,48)});
            auto* ptr = knob.get();
            self.widgets_[id] = ptr;
            self.wire_callbacks(id, ptr);
            self.root_.add_child(std::move(knob));
        }
        return choc::value::createString(id);
    });

    // createFader(id, orientation, parentId) OR createFader(id, x, y, w, h, orientation)
    register_bridge_function(api, "createFader", [&self, isNewApi](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto fader = std::make_unique<Fader>();
        fader->set_id(id);

        if (isNewApi(args)) {
            auto orient = args.get<std::string>(1, "vertical");
            auto pid = args.get<std::string>(2, "");
            if (orient == "horizontal") fader->set_orientation(Fader::Orientation::horizontal);
            auto* ptr = fader.get();
            self.widgets_[id] = ptr;
            self.wire_callbacks(id, ptr);
            self.resolve_parent(pid)->add_child(std::move(fader));
        } else {
            fader->set_bounds({(float)args.get<double>(1,0), (float)args.get<double>(2,0),
                              (float)args.get<double>(3,24), (float)args.get<double>(4,200)});
            auto orient = args.get<std::string>(5, "vertical");
            if (orient == "horizontal") fader->set_orientation(Fader::Orientation::horizontal);
            fader->set_label(id);
            auto* ptr = fader.get();
            self.widgets_[id] = ptr;
            self.wire_callbacks(id, ptr);
            self.root_.add_child(std::move(fader));
        }
        return choc::value::createString(id);
    });

    // createToggle(id, parentId) OR createToggle(id, x, y, w, h)
    register_bridge_function(api, "createToggle", [&self, isNewApi](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto toggle = std::make_unique<Toggle>();
        toggle->set_id(id);

        if (isNewApi(args)) {
            auto pid = args.get<std::string>(1, "");
            auto* ptr = toggle.get();
            self.widgets_[id] = ptr;
            self.wire_callbacks(id, ptr);
            self.resolve_parent(pid)->add_child(std::move(toggle));
        } else {
            toggle->set_bounds({(float)args.get<double>(1,0), (float)args.get<double>(2,0),
                               (float)args.get<double>(3,50), (float)args.get<double>(4,30)});
            toggle->set_label(id);
            auto* ptr = toggle.get();
            self.widgets_[id] = ptr;
            self.wire_callbacks(id, ptr);
            self.root_.add_child(std::move(toggle));
        }
        return choc::value::createString(id);
    });

    // createRangeSlider(id, parentId)
    register_bridge_function(api, "createRangeSlider", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto rs = std::make_unique<RangeSlider>();
        rs->set_id(id);
        auto* ptr = rs.get();
        self.widgets_[id] = ptr;
        self.wire_callbacks(id, ptr);
        self.resolve_parent(pid)->add_child(std::move(rs));
        return choc::value::createString(id);
    });

    // createIcon(id, type, parentId) - type: "image_upload", "send", "search", "close"
    register_bridge_function(api, "createIcon", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto type_str = args.get<std::string>(1, "image_upload");
        auto pid = args.get<std::string>(2, "");
        auto icon = std::make_unique<Icon>();
        icon->set_id(id);
        if (type_str == "send") icon->set_type(Icon::Type::send);
        else if (type_str == "search") icon->set_type(Icon::Type::search);
        else if (type_str == "close") icon->set_type(Icon::Type::close);
        else icon->set_type(Icon::Type::image_upload);
        self.widgets_[id] = icon.get();
        self.resolve_parent(pid)->add_child(std::move(icon));
        return choc::value::createString(id);
    });

    // createImage(id, parentId) - HTML <img> equivalent
    register_bridge_function(api, "createImage", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto img = std::make_unique<ImageView>(); img->set_id(id);
        self.widgets_[id] = img.get();
        self.resolve_parent(pid)->add_child(std::move(img));
        return choc::value::createString(id);
    });
}

void BridgeRegistrars::register_widget_factory_form_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // createCheckbox(id, parentId)
    register_bridge_function(api, "createCheckbox", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto cb = std::make_unique<Checkbox>(); cb->set_id(id);
        auto* ptr = cb.get(); self.widgets_[id] = ptr;
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        cb->on_change = [alive, engine, id](bool v) {
            dispatch_event(alive, engine, id, "change", v ? "1" : "0");
        };
        self.resolve_parent(pid)->add_child(std::move(cb));
        return choc::value::createString(id);
    });

    // createToggleButton(id, parentId)
    register_bridge_function(api, "createToggleButton", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto tb = std::make_unique<ToggleButton>(); tb->set_id(id);
        auto* ptr = tb.get(); self.widgets_[id] = ptr;
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        tb->on_toggle = [alive, engine, id](bool v) {
            dispatch_event(alive, engine, id, "toggle", v ? "1" : "0");
        };
        self.resolve_parent(pid)->add_child(std::move(tb));
        return choc::value::createString(id);
    });

    // createLabel(id, text, parentId) OR createLabel(id, text, x, y, w, h)
    register_bridge_function(api, "createLabel", [&self](choc::javascript::ArgumentList args) {
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
            self.widgets_[id] = label.get();
            self.root_.add_child(std::move(label));
        } else {
            auto pid = args.get<std::string>(2, "");
            self.widgets_[id] = label.get();
            self.resolve_parent(pid)->add_child(std::move(label));
        }
        return choc::value::createString(id);
    });
}

void BridgeRegistrars::register_widget_factory_container_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "createRow", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<View>(); v->set_id(id);
        v->flex().direction = FlexDirection::row;
        self.widgets_[id] = v.get();
        self.resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createCol", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<View>(); v->set_id(id);
        v->flex().direction = FlexDirection::column;
        self.widgets_[id] = v.get();
        self.resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createModal", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto v = std::make_unique<ModalOverlay>(); v->set_id(id);
        v->flex().direction = FlexDirection::column;
        auto* modal = v.get();
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        modal->on_dismiss = [alive, engine, modal, id]() {
            if (modal) {
                modal->set_visible(false);
            }
            dispatch_event(alive, engine, id, "dismiss", "0");
        };
        self.widgets_[id] = v.get();
        self.resolve_parent(pid)->add_child(std::move(v));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createPanel", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<Panel>(); p->set_id(id);
        self.widgets_[id] = p.get();
        self.resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });
}

void BridgeRegistrars::register_widget_factory_composite_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "createMeter", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto o = args.get<std::string>(1, "vertical");
        auto pid = args.get<std::string>(2, "");
        auto m = std::make_unique<Meter>(); m->set_id(id);
        if (o == "horizontal") m->set_orientation(Meter::Orientation::horizontal);
        self.widgets_[id] = m.get(); self.resolve_parent(pid)->add_child(std::move(m));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createXYPad", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<XYPad>(); p->set_id(id);
        self.widgets_[id] = p.get(); self.resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createWaveform", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto w = std::make_unique<WaveformView>(); w->set_id(id);
        self.widgets_[id] = w.get(); self.resolve_parent(pid)->add_child(std::move(w));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createSpectrum", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto s = std::make_unique<SpectrumView>(); s->set_id(id);
        self.widgets_[id] = s.get(); self.resolve_parent(pid)->add_child(std::move(s));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createCombo", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto c = std::make_unique<ComboBox>(); c->set_id(id);
        auto* ptr = c.get(); self.widgets_[id] = ptr;
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        c->on_change = [alive, engine, id](int idx) {
            dispatch_event(alive, engine, id, "select", std::to_string(idx));
        };
        self.resolve_parent(pid)->add_child(std::move(c));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createProgress", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<ProgressBar>(); p->set_id(id);
        self.widgets_[id] = p.get(); self.resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createScrollView", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto s = std::make_unique<ScrollView>(); s->set_id(id);
        self.widgets_[id] = s.get(); self.resolve_parent(pid)->add_child(std::move(s));
        return choc::value::createString(id);
    });

    // A layout box for a platform-native child view (WebView / native text
    // field / video layer). Materializes empty — JS can't mint an OS view
    // handle — so a C++ host binds the native child by id afterwards via
    // `dynamic_cast<NativeViewHost*>(bridge.widget(id))->set_native_child(...)`.
    register_bridge_function(api, "createNativeView", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto nv = std::make_unique<NativeViewHost>(); nv->set_id(id);
        self.widgets_[id] = nv.get(); self.resolve_parent(pid)->add_child(std::move(nv));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createListBox", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto lb = std::make_unique<ListBox>(); lb->set_id(id);
        auto* ptr = lb.get(); self.widgets_[id] = ptr;
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        lb->on_select = [alive, engine, id](int idx) {
            dispatch_event(alive, engine, id, "select", std::to_string(idx));
        };
        lb->on_activate = [alive, engine, id](int idx) {
            dispatch_event(alive, engine, id, "activate", std::to_string(idx));
        };
        self.resolve_parent(pid)->add_child(std::move(lb));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createVirtualList", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto list = std::make_unique<VirtualList>(); list->set_id(id);
        auto* ptr = list.get(); self.widgets_[id] = ptr;
        self.wire_callbacks(id, ptr);
        self.resolve_parent(pid)->add_child(std::move(list));
        return choc::value::createString(id);
    });

    register_bridge_function(api, "createVirtualGrid", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto grid = std::make_unique<VirtualGrid>(); grid->set_id(id);
        auto* ptr = grid.get(); self.widgets_[id] = ptr;
        self.wire_callbacks(id, ptr);
        self.resolve_parent(pid)->add_child(std::move(grid));
        return choc::value::createString(id);
    });

}

void BridgeRegistrars::register_widget_factory_text_editor_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "createTextEditor", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, ""); auto pid = args.get<std::string>(1, "");
        auto ed = std::make_unique<TextEditor>(); ed->set_id(id);
        auto* ptr = ed.get(); self.widgets_[id] = ptr;
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        ed->on_escape = [alive, engine, id]() {
            dispatch_event(alive, engine, id, "escape", "0");
        };
        ed->on_return = [alive, engine, id](const std::string& text) {
            std::string e; for (char c : text) { if (c=='\'') e+= "\\'"; else if (c=='\n') e+= "\\n"; else e+= c; }
            dispatch_event(alive, engine, id, "return", "'" + e + "'");
        };
        ed->on_change = [alive, engine, id](const std::string& text) {
            std::string e; for (char c : text) { if (c=='\'') e+= "\\'"; else if (c=='\n') e+= "\\n"; else e+= c; }
            dispatch_event(alive, engine, id, "change", "'" + e + "'");
        };
        self.resolve_parent(pid)->add_child(std::move(ed));
        return choc::value::createString(id);
    });
}

void BridgeRegistrars::register_widget_factory_design_system_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};
    auto tone_from = [](const std::string& s) {
        if (s == "info") return Tone::info;
        if (s == "success") return Tone::success;
        if (s == "warning") return Tone::warning;
        if (s == "danger") return Tone::danger;
        return Tone::neutral;
    };

    // createBadge(id, text, tone, parentId) — Ink & Signal status pill.
    register_bridge_function(api, "createBadge", [&self, tone_from](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto text = args.get<std::string>(1, "");
        auto tone = args.get<std::string>(2, "neutral");
        auto pid = args.get<std::string>(3, "");
        auto b = std::make_unique<Badge>(text, tone_from(tone));
        b->set_id(id);
        self.widgets_[id] = b.get();
        self.resolve_parent(pid)->add_child(std::move(b));
        return choc::value::createString(id);
    });

    // createStepper(id, parentId) — [-] value [+] numeric nudge.
    register_bridge_function(api, "createStepper", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto s = std::make_unique<Stepper>();
        s->set_id(id);
        auto* ptr = s.get();
        self.widgets_[id] = ptr;
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        s->on_change = [alive, engine, id](double v) {
            dispatch_event(alive, engine, id, "change", std::to_string(v));
        };
        self.resolve_parent(pid)->add_child(std::move(s));
        return choc::value::createString(id);
    });

    // createPan(id, parentId) — bipolar 1-D pan (-1..+1).
    register_bridge_function(api, "createPan", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pid = args.get<std::string>(1, "");
        auto p = std::make_unique<PanControl>();
        p->set_id(id);
        auto* ptr = p.get();
        self.widgets_[id] = ptr;
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        p->on_change = [alive, engine, id](float v) {
            dispatch_event(alive, engine, id, "change", std::to_string(v));
        };
        self.resolve_parent(pid)->add_child(std::move(p));
        return choc::value::createString(id);
    });
}

} // namespace pulp::view
