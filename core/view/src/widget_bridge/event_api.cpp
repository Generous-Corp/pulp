// widget_bridge/event_api.cpp - event registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"
#include "bridge_dispatch.hpp"

#include <pulp/view/gesture.hpp>
#include <pulp/platform/popup_menu.hpp>

#include <choc/text/choc_JSON.h>

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <iostream>
#include <memory>
#include <string>

namespace pulp::view {

namespace {

Point local_to_root(const View* view, Point point) {
    for (auto* cur = view; cur; cur = cur->parent()) {
        point.x += cur->bounds().x;
        point.y += cur->bounds().y;
    }
    return point;
}

std::string point_payload(const View* owner, Point local) {
    const Point root = local_to_root(owner, local);
    return "clientX:" + std::to_string(root.x) + "," +
           "clientY:" + std::to_string(root.y) + "," +
           "offsetX:" + std::to_string(local.x) + "," +
           "offsetY:" + std::to_string(local.y);
}

void dispatch_gesture_js(const std::shared_ptr<std::atomic<bool>>& alive,
                         ScriptEngine* engine,
                         const std::string& id,
                         const std::string& event_name,
                         const std::string& data) {
    dispatch_event(alive, engine, id, event_name, "{" + data + "}");
}

} // namespace

void BridgeRegistrars::register_hover_event_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // registerHover(id) - enables "mouseenter"/"mouseleave" JS callbacks (CSS :hover).
    register_bridge_function(api, "registerHover", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = self.widgets_.find(id);
        if (it != self.widgets_.end()) {
            auto alive = self.callback_alive_;
            auto* engine = &self.engine_;
            it->second->on_hover_enter = [alive, engine, id]() {
                dispatch_event(alive, engine, id, "mouseenter", "0");
            };
            it->second->on_hover_leave = [alive, engine, id]() {
                dispatch_event(alive, engine, id, "mouseleave", "0");
            };
        }
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_pointer_event_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // registerClick(id) - enables "click" event dispatch for any widget.
    register_bridge_function(api, "registerClick", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = self.widgets_.find(id);
        if (it != self.widgets_.end()) {
            auto alive = self.callback_alive_;
            auto* engine = &self.engine_;
            it->second->on_click = [alive, engine, id]() {
                dispatch_event(alive, engine, id, "click", "0");
            };
        }
        return choc::value::Value();
    });

    // claimOverlay(id) / releaseOverlay(id) - generalized overlay click routing.
    register_bridge_function(api, "claimOverlay", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = self.widgets_.find(id);
        if (it != self.widgets_.end() && it->second) {
            // Install a JS-visible dismiss callback so React overlay consumers
            // can flip setOpen(false) when the framework dismisses the overlay.
            auto alive = self.callback_alive_;
            auto* engine = &self.engine_;
            it->second->on_overlay_dismissed = [alive, engine, id]() {
                dispatch_event(alive, engine, id, "dismiss", "0");
            };
            it->second->claim_overlay();
        }
        return choc::value::Value();
    });
    register_bridge_function(api, "releaseOverlay", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = self.widgets_.find(id);
        if (it != self.widgets_.end() && it->second) {
            // JS-driven release, typically React unmount. Clear the dismiss
            // callback first so a later ESC/outside-click cannot re-fire it.
            it->second->on_overlay_dismissed = nullptr;
            it->second->release_overlay();
        }
        return choc::value::Value();
    });

    // registerPointer(id) - enables pointer event dispatch for a widget.
    register_bridge_function(api, "registerPointer", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        // Idempotency: re-renders re-issue registerPointer for the same id.
        // Without this gate each call wraps the previous on_pointer_event,
        // stacking N lambdas and multiplying dispatch cost by render count.
        if (!self.claim_pointer_registration(id)) {
            return choc::value::Value();
        }
        if (const char* dbg = std::getenv("PULP_DEBUG_POINTER"); dbg && *dbg) {
            std::cerr << "[bridge] registerPointer id=" << id << " widgets_.has=" << (self.widgets_.count(id) ? "yes" : "NO") << "\n";
        }
        auto it = self.widgets_.find(id);
        if (it != self.widgets_.end()) {
            auto* w = it->second.view;
            auto alive = self.callback_alive_;
            auto* engine = &self.engine_;
            auto previous_pointer = w->on_pointer_event;
            w->on_pointer_event = [alive, engine, id, previous_pointer](const MouseEvent& me) {
                if (previous_pointer) {
                    previous_pointer(me);
                }
                if (me.is_wheel) {
                    return;
                }
                // This channel carries the press/release edges only. A move —
                // `MousePhase::drag` (button held) or `hover` (button up) —
                // reaches JS as `pointermove` through `on_drag` /
                // `on_pointer_move` below, and the two must not both report it.
                //
                // The type is otherwise inferred from `is_down`, which is what
                // `MousePhase::automatic` asks for and what every press/release
                // caller wants. But a drag tick carries `is_down == true` (the
                // button IS still held), so inferring from it alone reported
                // EVERY drag sample as a fresh `pointerdown`. A handler that
                // latches its gesture origin on press — the standard knob idiom,
                // `y0 = e.clientY` — re-latched on every sample, so its delta
                // was always ~0 and the control never moved under a drag. A
                // hover sample would likewise have reported a phantom
                // `pointerup`.
                if (me.phase == MousePhase::drag || me.phase == MousePhase::hover) {
                    return;
                }
                std::string type;
                if (me.is_down) type = "pointerdown";
                else type = "pointerup";
                if (me.is_cancelled) type = "pointercancel";

                // W3C MouseEvent.button: left=0, middle=1, right=2.
                int w3c_button = 0;
                switch (me.button) {
                    case MouseButton::left:   w3c_button = 0; break;
                    case MouseButton::middle: w3c_button = 1; break;
                    case MouseButton::right:  w3c_button = 2; break;
                    case MouseButton::none:   w3c_button = 0; break;
                }
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
                    "button:" + std::to_string(w3c_button) + ","
                    "ctrlKey:" + (me.isCtrlDown() ? "true" : "false") + ","
                    "shiftKey:" + (me.isShiftDown() ? "true" : "false") + ","
                    "altKey:" + (me.isAltDown() ? "true" : "false") + ","
                    "metaKey:" + (me.isCmdDown() ? "true" : "false") +
                    "}";

                if (const char* dbg = std::getenv("PULP_DEBUG_POINTER"); dbg && *dbg) {
                    std::cerr << "[bridge] pointer " << type << " id=" << id << "\n";
                }
                dispatch_event(alive, engine, id, type, data);

                std::string mouse_type;
                if (type == "pointerdown") mouse_type = "mousedown";
                else if (type == "pointerup") mouse_type = "mouseup";
                else if (type == "pointercancel") mouse_type = "mouseup";
                if (!mouse_type.empty()) {
                    dispatch_event(alive, engine, id, mouse_type, data);
                    dispatch_event(alive, engine, "__global__", mouse_type, data);
                }
            };

            // W3C PointerEvents: forward drag as pointermove.
            w->on_drag = [alive, engine, id, w](Point pos) {
                float wx = pos.x, wy = pos.y;
                for (View* cur = w; cur; cur = cur->parent()) {
                    wx += cur->bounds().x;
                    wy += cur->bounds().y;
                }
                std::string data = "{"
                    "clientX:" + std::to_string(wx) + ","
                    "clientY:" + std::to_string(wy) + ","
                    "offsetX:" + std::to_string(pos.x) + ","
                    "offsetY:" + std::to_string(pos.y) + ","
                    "pointerId:0,pointerType:'mouse',isPrimary:true,"
                    "button:0,buttons:1}";
                if (const char* dbg = std::getenv("PULP_DEBUG_POINTER"); dbg && *dbg) {
                    std::cerr << "[bridge] drag id=" << id << " @(" << pos.x << "," << pos.y << ")\n";
                }
                dispatch_event(alive, engine, id, "pointermove", data);
                dispatch_event(alive, engine, id, "mousemove", data);
                dispatch_event(alive, engine, "__global__", "mousemove", data);
            };

            // Identity-preserving pointermove for iOS multi-touch.
            w->on_pointer_move = [alive, engine, id](const MouseEvent& me) {
                std::string data = "{"
                    "clientX:" + std::to_string(me.window_position.x) + ","
                    "clientY:" + std::to_string(me.window_position.y) + ","
                    "offsetX:" + std::to_string(me.position.x) + ","
                    "offsetY:" + std::to_string(me.position.y) + ","
                    "pointerId:" + std::to_string(me.pointer_id) + ","
                    "pointerType:'" + std::string(me.pointerTypeString()) + "',"
                    "isPrimary:" + (me.isPrimary() ? "true" : "false") + ","
                    "pressure:" + std::to_string(me.pressure) + ","
                    "button:0,buttons:1}";
                dispatch_event(alive, engine, id, "pointermove", data);
                dispatch_event(alive, engine, id, "mousemove", data);
                dispatch_event(alive, engine, "__global__", "mousemove", data);
            };
        }
        return choc::value::Value();
    });

    // registerGesture(id) - enables gesture event dispatch for a widget.
    register_bridge_function(api, "registerGesture", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto it = self.widgets_.find(id);
        if (it != self.widgets_.end()) {
            auto alive = self.callback_alive_;
            auto* engine = &self.engine_;
            it->second->on_gesture_cb = [alive, engine, id](const GestureEvent& ge) {
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
                dispatch_event(alive, engine, id, type, data);
            };
        }
        return choc::value::Value();
    });

    auto add_recognizer_once = [&self](
            const std::string& id,
            const std::string& key,
            std::unique_ptr<GestureRecognizer> recognizer) {
        if (!recognizer) return;
        auto it = self.widgets_.find(id);
        if (it == self.widgets_.end() || !it->second)
            return;
        if (!self.claim_gesture_registration(id, key))
            return;
        it->second->add_gesture_recognizer(std::move(recognizer));
    };

    auto allow_pinch_rotate_simultaneous = [&self](const std::string& id) {
        auto it = self.widgets_.find(id);
        if (it == self.widgets_.end() || !it->second) return;
        PinchRecognizer* pinch = nullptr;
        RotateRecognizer* rotate = nullptr;
        const size_t count = it->second->gesture_recognizer_count();
        for (size_t i = 0; i < count; ++i) {
            auto* recognizer = it->second->gesture_recognizer_at(i);
            if (!recognizer) continue;
            if (!pinch) pinch = dynamic_cast<PinchRecognizer*>(recognizer);
            if (!rotate) rotate = dynamic_cast<RotateRecognizer*>(recognizer);
        }
        if (pinch && rotate)
            pinch->allow_simultaneous_with(*rotate);
    };

    register_bridge_function(api, "registerTapGesture", [&self, add_recognizer_once](choc::javascript::ArgumentList args) mutable {
        auto id = args.get<std::string>(0, "");
        auto tap = std::make_unique<TapRecognizer>(1);
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        tap->on_ended = [alive, engine, id](GestureRecognizer& recognizer) {
            auto& tap_ref = static_cast<TapRecognizer&>(recognizer);
            dispatch_gesture_js(alive, engine, id, "tap",
                point_payload(recognizer.owner(), tap_ref.position()) + ",tapCount:1");
        };
        add_recognizer_once(id, "tap", std::move(tap));
        return choc::value::Value();
    });

    register_bridge_function(api, "registerDoubleTapGesture", [&self, add_recognizer_once](choc::javascript::ArgumentList args) mutable {
        auto id = args.get<std::string>(0, "");
        auto tap = std::make_unique<TapRecognizer>(2);
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        tap->on_ended = [alive, engine, id](GestureRecognizer& recognizer) {
            auto& tap_ref = static_cast<TapRecognizer&>(recognizer);
            dispatch_gesture_js(alive, engine, id, "doubletap",
                point_payload(recognizer.owner(), tap_ref.position()) + ",tapCount:2");
        };
        add_recognizer_once(id, "doubletap", std::move(tap));
        return choc::value::Value();
    });

    register_bridge_function(api, "registerLongPressGesture", [&self, add_recognizer_once](choc::javascript::ArgumentList args) mutable {
        auto id = args.get<std::string>(0, "");
        auto long_press = std::make_unique<LongPressRecognizer>();
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        long_press->on_began = [alive, engine, id](GestureRecognizer& recognizer) {
            auto& long_press_ref = static_cast<LongPressRecognizer&>(recognizer);
            dispatch_gesture_js(alive, engine, id, "longpress",
                point_payload(recognizer.owner(), long_press_ref.position()));
        };
        add_recognizer_once(id, "longpress", std::move(long_press));
        return choc::value::Value();
    });

    register_bridge_function(api, "registerPanGesture", [&self, add_recognizer_once](choc::javascript::ArgumentList args) mutable {
        auto id = args.get<std::string>(0, "");
        auto pan = std::make_unique<PanRecognizer>();
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        auto dispatch_pan = [alive, engine, id](const char* event_name,
                                                GestureRecognizer& recognizer) {
            auto& pan_ref = static_cast<PanRecognizer&>(recognizer);
            const auto t = pan_ref.translation();
            const auto v = pan_ref.velocity();
            dispatch_gesture_js(alive, engine, id, event_name,
                "translationX:" + std::to_string(t.x) + "," +
                "translationY:" + std::to_string(t.y) + "," +
                "velocityX:" + std::to_string(v.x) + "," +
                "velocityY:" + std::to_string(v.y));
        };
        pan->on_began = [dispatch_pan](GestureRecognizer& recognizer) {
            dispatch_pan("panstart", recognizer);
        };
        pan->on_changed = [dispatch_pan](GestureRecognizer& recognizer) {
            dispatch_pan("panchange", recognizer);
        };
        pan->on_ended = [dispatch_pan](GestureRecognizer& recognizer) {
            dispatch_pan("panend", recognizer);
        };
        add_recognizer_once(id, "pan", std::move(pan));
        return choc::value::Value();
    });

    register_bridge_function(api, "registerSwipeGesture", [&self, add_recognizer_once](choc::javascript::ArgumentList args) mutable {
        auto id = args.get<std::string>(0, "");
        auto swipe = std::make_unique<SwipeRecognizer>();
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        swipe->on_ended = [alive, engine, id](GestureRecognizer& recognizer) {
            auto& swipe_ref = static_cast<SwipeRecognizer&>(recognizer);
            const auto t = swipe_ref.translation();
            const auto v = swipe_ref.velocity();
            dispatch_gesture_js(alive, engine, id, "swipe",
                "translationX:" + std::to_string(t.x) + "," +
                "translationY:" + std::to_string(t.y) + "," +
                "velocityX:" + std::to_string(v.x) + "," +
                "velocityY:" + std::to_string(v.y));
        };
        add_recognizer_once(id, "swipe", std::move(swipe));
        return choc::value::Value();
    });

    register_bridge_function(api, "registerFlingGesture", [&self, add_recognizer_once](choc::javascript::ArgumentList args) mutable {
        auto id = args.get<std::string>(0, "");
        auto fling = std::make_unique<FlingRecognizer>();
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        fling->on_ended = [alive, engine, id](GestureRecognizer& recognizer) {
            auto& fling_ref = static_cast<FlingRecognizer&>(recognizer);
            const auto t = fling_ref.translation();
            const auto v = fling_ref.velocity();
            dispatch_gesture_js(alive, engine, id, "fling",
                "translationX:" + std::to_string(t.x) + "," +
                "translationY:" + std::to_string(t.y) + "," +
                "velocityX:" + std::to_string(v.x) + "," +
                "velocityY:" + std::to_string(v.y));
        };
        add_recognizer_once(id, "fling", std::move(fling));
        return choc::value::Value();
    });

    register_bridge_function(api, "registerPinchGesture", [&self, add_recognizer_once, allow_pinch_rotate_simultaneous](choc::javascript::ArgumentList args) mutable {
        auto id = args.get<std::string>(0, "");
        auto pinch = std::make_unique<PinchRecognizer>();
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        auto dispatch_pinch = [alive, engine, id](const char* event_name,
                                                  GestureRecognizer& recognizer) {
            auto& pinch_ref = static_cast<PinchRecognizer&>(recognizer);
            dispatch_gesture_js(alive, engine, id, event_name,
                point_payload(recognizer.owner(), pinch_ref.center()) + "," +
                "scale:" + std::to_string(pinch_ref.scale()) + "," +
                "deltaScale:" + std::to_string(pinch_ref.delta_scale()));
        };
        pinch->on_began = [dispatch_pinch](GestureRecognizer& recognizer) {
            dispatch_pinch("pinchstart", recognizer);
        };
        pinch->on_changed = [dispatch_pinch](GestureRecognizer& recognizer) {
            dispatch_pinch("pinchchange", recognizer);
        };
        pinch->on_ended = [dispatch_pinch](GestureRecognizer& recognizer) {
            dispatch_pinch("pinchend", recognizer);
        };
        add_recognizer_once(id, "pinch", std::move(pinch));
        allow_pinch_rotate_simultaneous(id);
        return choc::value::Value();
    });

    register_bridge_function(api, "registerRotateGesture", [&self, add_recognizer_once, allow_pinch_rotate_simultaneous](choc::javascript::ArgumentList args) mutable {
        auto id = args.get<std::string>(0, "");
        auto rotate = std::make_unique<RotateRecognizer>();
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        auto dispatch_rotate = [alive, engine, id](const char* event_name,
                                                   GestureRecognizer& recognizer) {
            auto& rotate_ref = static_cast<RotateRecognizer&>(recognizer);
            dispatch_gesture_js(alive, engine, id, event_name,
                point_payload(recognizer.owner(), rotate_ref.center()) + "," +
                "rotation:" + std::to_string(rotate_ref.rotation()) + "," +
                "deltaRotation:" + std::to_string(rotate_ref.delta_rotation()));
        };
        rotate->on_began = [dispatch_rotate](GestureRecognizer& recognizer) {
            dispatch_rotate("rotatestart", recognizer);
        };
        rotate->on_changed = [dispatch_rotate](GestureRecognizer& recognizer) {
            dispatch_rotate("rotatechange", recognizer);
        };
        rotate->on_ended = [dispatch_rotate](GestureRecognizer& recognizer) {
            dispatch_rotate("rotateend", recognizer);
        };
        add_recognizer_once(id, "rotate", std::move(rotate));
        allow_pinch_rotate_simultaneous(id);
        return choc::value::Value();
    });

    // nativeSetPointerCapture(id, pointerId).
    register_bridge_function(api, "nativeSetPointerCapture", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pointerId = static_cast<int>(args.get<double>(1, 0));
        auto it = self.widgets_.find(id);
        if (it != self.widgets_.end()) {
            it->second->set_pointer_capture(pointerId);
            dispatch_event(self.engine_, id, "gotpointercapture",
                           "{pointerId:" + std::to_string(pointerId) + "}");
        }
        return choc::value::Value();
    });

    // nativeReleasePointerCapture(id, pointerId).
    register_bridge_function(api, "nativeReleasePointerCapture", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto pointerId = static_cast<int>(args.get<double>(1, 0));
        auto it = self.widgets_.find(id);
        if (it != self.widgets_.end()) {
            it->second->release_pointer_capture(pointerId);
            dispatch_event(self.engine_, id, "lostpointercapture",
                           "{pointerId:" + std::to_string(pointerId) + "}");
        }
        return choc::value::Value();
    });

    // enableInspectClick() - sets up Cmd+click detection on all registered widgets.
    register_bridge_function(api, "enableInspectClick", [&self](choc::javascript::ArgumentList) {
        auto alive = self.callback_alive_;
        auto* engine = &self.engine_;
        self.root_.on_global_click = [alive, engine](const std::string& id, uint16_t mods) {
            bool cmd = (mods & (0x10 | 0x08)) != 0;
            if (cmd) {
                dispatch_event(alive, engine, "__inspect__", "click", js_string_literal(id));
            }
        };
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_wheel_event_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // registerWheel(id) - enable wheel event dispatch for scroll/zoom.
    register_bridge_function(api, "registerWheel", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        // Idempotency: re-renders re-issue registerWheel for the same id.
        // Without this gate each call wraps the previous on_pointer_event,
        // stacking N lambdas and multiplying dispatch cost by render count.
        if (!self.claim_wheel_registration(id)) {
            return choc::value::Value();
        }
        auto it = self.widgets_.find(id);
        if (it != self.widgets_.end()) {
            auto* w = it->second.view;
            auto alive = self.callback_alive_;
            auto* engine = &self.engine_;
            auto previous_pointer = w->on_pointer_event;
            w->on_pointer_event = [alive, engine, id, previous_pointer](const MouseEvent& me) {
                if (previous_pointer) {
                    previous_pointer(me);
                }
                if (!me.is_wheel) {
                    return;
                }
                std::string data = "{"
                    "deltaX:" + std::to_string(me.scroll_delta_x) + ","
                    "deltaY:" + std::to_string(me.scroll_delta_y) + ","
                    "clientX:" + std::to_string(me.window_position.x) + ","
                    "clientY:" + std::to_string(me.window_position.y) +
                    "}";
                dispatch_event(alive, engine, id, "wheel", data);
            };
        }
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_context_menu_event_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // registerContextMenu(id, callbackName)
    register_bridge_function(api, "registerContextMenu", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto cb = args.get<std::string>(1, "");
        auto* v = self.widget(id);
        if (v && !cb.empty()) {
            auto alive = self.callback_alive_;
            auto* engine = &self.engine_;
            v->on_context_menu = [alive, engine, cb](Point pos) {
                safe_dispatch_eval(alive, engine,
                    cb + "(" + std::to_string(pos.x) + "," + std::to_string(pos.y) + ")",
                    "context menu");
            };
        }
        return choc::value::Value();
    });

    // showContextMenu(itemsJSON, x, y) -> selected id or -1.
    register_bridge_function(api, "showContextMenu", [](choc::javascript::ArgumentList args) {
        auto json = args.get<std::string>(0, "");
        auto x = args.get<double>(1, 0);
        auto y = args.get<double>(2, 0);

        platform::PopupMenu menu;
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

    // registerShortcut(key, modifiers, callbackName)
    register_bridge_function(api, "registerShortcut", [&self](choc::javascript::ArgumentList args) {
        auto key = args.get<int>(0, 0);
        auto mods = args.get<int>(1, 0);
        auto cb = args.get<std::string>(2, "");
        if (!cb.empty()) {
            self.shortcuts_.push_back({static_cast<KeyCode>(key),
                                  static_cast<uint16_t>(mods), cb});
        }
        return choc::value::Value();
    });
}

void BridgeRegistrars::register_drop_event_api(WidgetBridge& self) {
    BridgeApiContext api{self.engine_};

    // Drag-and-drop: register JS callback for file/text drops.
    register_bridge_function(api, "registerDrop", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<std::string>(0, "");
        auto cb = args.get<std::string>(1, "");
        auto* v = self.widget(id);
        if (v && !cb.empty()) {
            auto alive = self.callback_alive_;
            auto* engine = &self.engine_;
            v->on_drop = [alive, engine, cb](const std::string& type, const std::string& data, float x, float y) {
                std::string safe_data;
                for (char c : data) {
                    if (c == '\'') safe_data += "\\'";
                    else if (c == '\n') safe_data += "\\n";
                    else safe_data += c;
                }
                safe_dispatch_eval(alive, engine,
                    cb + "('" + type + "','" + safe_data + "'," +
                    std::to_string(x) + "," + std::to_string(y) + ")",
                    "drop");
            };
        }
        return choc::value::Value();
    });
}

} // namespace pulp::view
