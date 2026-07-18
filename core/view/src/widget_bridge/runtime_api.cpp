// widget_bridge/runtime_api.cpp - frame, timer, and motion runtime registrations for WidgetBridge.

#include <pulp/view/widget_bridge.hpp>
#include "api_registry.hpp"

#include <pulp/view/motion.hpp>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>
#include <vector>

namespace pulp::view {

void BridgeRegistrars::register_runtime_api(WidgetBridge& self) {
    // __requestFrame__ - requestAnimationFrame implementation.
    // JS side stores callbacks in __frameCallbacks__ map, passes ID to C++.
    // C++ stores pending IDs and invokes them on next frame via __invokeFrame__.
    // Shared pending frame callback IDs.
    if (!self.frame_preamble_loaded_) {
        self.frame_preamble_loaded_ = true;
        self.engine_.evaluate(
            "var __frameCallbacks__ = {};"
            "var __frameNextId__ = 1;"
            "function __invokeFrame__(id) {"
            "  var fn = __frameCallbacks__[id];"
            "  if (fn) { delete __frameCallbacks__[id]; fn(); }"
            "}"
            // Timer registry for native setTimeout / setInterval. Callbacks
            // live in JS (CHOC's NativeFunction can't carry JSValue
            // arguments); native side tracks deadlines + repeat semantics
            // and pings these helpers when a timer expires.
            "var __timerCallbacks__ = Object.create(null);"
            "var __timerNextId__ = 1;"
            "function __invokeTimer__(id) {"
            "  var entry = __timerCallbacks__[id];"
            "  if (!entry) return;"
            "  if (!entry.repeat) delete __timerCallbacks__[id];"
            "  try { entry.fn(); } catch (e) {"
            "    if (typeof console !== 'undefined' && console.error)"
            "      console.error('timer:', e);"
            "  }"
            "}"
        );
    }

    BridgeApiContext api{self.engine_};

    register_bridge_function(api, "__requestFrame__", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<int>(0, 0);
        if (id > 0) {
            self.pending_frame_ids_.push_back(id);
            // Signal the host so the next paint runs and service_frame_callbacks()
            // drains the queue. Without this,
            // requestAnimationFrame queues a callback but never asks the
            // host for a frame, so the canvas never repaints.
            self.request_repaint();
        }
        return choc::value::createInt32(id);
    });

    register_bridge_function(api, "__cancelFrame__", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<int>(0, 0);
        auto it = std::find(self.pending_frame_ids_.begin(), self.pending_frame_ids_.end(), id);
        if (it != self.pending_frame_ids_.end()) self.pending_frame_ids_.erase(it);
        return choc::value::Value();
    });

    register_bridge_function(api, "__flushFrames__", [&self](choc::javascript::ArgumentList) {
        auto ids = self.pending_frame_ids_;
        self.pending_frame_ids_.clear();
        if (ids.empty()) {
            return choc::value::Value();
        }
        // Invoke each callback under its own ambient provenance. The script identity (set via
        // `load_script(code, script_id)` / `set_active_script_id`) is
        // prefixed onto the callback id so a published value emitted
        // from inside an rAF body inherits source_id =
        // "<script_id>:<callback_id>". One callback at a time so the
        // ambient slot is correct per-invocation.
        const bool stamp = !self.active_script_id_.empty();
        // RAII guard so an exception in `__invokeFrame__` doesn't leave
        // stale ambient provenance behind, corrupting attribution for
        // every subsequent publish until something else clears it.
        struct AmbientGuard {
            bool active;
            ~AmbientGuard() { if (active) motion::clear_ambient_provenance(); }
        };
        for (auto id : ids) {
            AmbientGuard guard{stamp};
            if (stamp) {
                motion::Provenance p;
                p.source_kind = "rAF";
                p.source_id = self.active_script_id_ + ":" + std::to_string(id);
                motion::set_ambient_provenance(std::move(p));
            }
            std::string call = "__invokeFrame__(" + std::to_string(id) + ");void 0;";
            self.engine_.evaluate(call);
            // guard's dtor runs on normal and exception paths.
        }
        return choc::value::Value();
    });

    // JS-side bridge for the motion publish channel and ambient provenance slot.
    //
    // Exposes:
    //   motion.publishValue(viewName, metricName, value)
    //   motion.setProvenance(kind, id)         // sticky
    //   motion.clearProvenance()
    //
    // The C++ side already off-by-defaults when tracing is disabled, so
    // calling these in production code is cheap. Design-import codegen
    // can emit `motion.setProvenance("design-import", "figma:Card/Hover")`
    // alongside its `view.animate({...})` so the emitted trace carries
    // the vendor + node identity. rAF callbacks pick up
    // `source_kind="rAF"` automatically via the ambient slot set by
    // `__flushFrames__` when an active script id is configured.
    register_bridge_function(api, "__motionPublishValue__",
        [](choc::javascript::ArgumentList args) {
            auto view = args.get<std::string>(0, "");
            auto metric = args.get<std::string>(1, "");
            auto value = args.get<double>(2, 0.0);
            if (!view.empty() && !metric.empty()) {
                motion::publish_value(std::move(view), std::move(metric), value);
            }
            return choc::value::Value();
        });

    register_bridge_function(api, "__motionSetProvenance__",
        [](choc::javascript::ArgumentList args) {
            motion::Provenance p;
            p.source_kind = args.get<std::string>(0, "");
            p.source_id = args.get<std::string>(1, "");
            // Optional file + line for callers that want to point at a
            // specific JSX/JS source line.
            p.source_file = args.get<std::string>(2, "");
            p.source_line = args.get<int>(3, 0);
            motion::set_ambient_provenance(std::move(p));
            return choc::value::Value();
        });

    register_bridge_function(api, "__motionClearProvenance__",
        [](choc::javascript::ArgumentList) {
            motion::clear_ambient_provenance();
            return choc::value::Value();
        });

    // Install the JS-side `motion` global wrapping the natives.
    // Idempotent - re-evaluating the same definition is a no-op.
    self.engine_.evaluate(
        "if (typeof globalThis.motion === 'undefined') {"
        "  globalThis.motion = {"
        "    publishValue: function(view, metric, value) {"
        "      return __motionPublishValue__(String(view), String(metric), Number(value));"
        "    },"
        "    setProvenance: function(kind, id, file, line) {"
        "      return __motionSetProvenance__(String(kind || ''), String(id || ''),"
        "                                     String(file || ''), Number(line || 0));"
        "    },"
        "    set_provenance: function(kind, id, file, line) {"
        "      return globalThis.motion.setProvenance(kind, id, file, line);"
        "    },"
        "    clearProvenance: function() { return __motionClearProvenance__(); },"
        "  };"
        "}"
        "void 0;"
    );

    // Native setTimeout / setInterval scheduling. JS-side setTimeout/setInterval
    // generate the id and stash the callback in
    // __timerCallbacks__; native tracks (id, deadline, repeat, interval)
    // so service_frame_callbacks() can fire expired timers without a
    // consumer-side shim.
    register_bridge_function(api, "__scheduleTimer__", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<int>(0, 0);
        auto delay = args.get<double>(1, 0.0);
        auto repeat = args.get<bool>(2, false);
        if (id <= 0) return choc::value::createInt32(0);
        if (delay < 0.0) delay = 0.0;
        auto delay_ms = std::chrono::milliseconds(static_cast<long long>(delay));
        WidgetBridge::PendingTimer t;
        t.id = id;
        t.deadline = std::chrono::steady_clock::now() + delay_ms;
        t.interval = delay_ms;
        t.repeating = repeat;
        // Replace any prior schedule for this id (shouldn't happen, but
        // setInterval/setTimeout share the JS-side id allocator and a
        // misbehaving consumer might double-schedule).
        auto it = std::find_if(self.pending_timers_.begin(), self.pending_timers_.end(),
            [id](const WidgetBridge::PendingTimer& p){ return p.id == id; });
        if (it != self.pending_timers_.end()) *it = t;
        else self.pending_timers_.push_back(t);
        return choc::value::createInt32(id);
    });

    register_bridge_function(api, "__cancelTimer__", [&self](choc::javascript::ArgumentList args) {
        auto id = args.get<int>(0, 0);
        auto it = std::remove_if(self.pending_timers_.begin(), self.pending_timers_.end(),
            [id](const WidgetBridge::PendingTimer& p){ return p.id == id; });
        self.pending_timers_.erase(it, self.pending_timers_.end());
        return choc::value::Value();
    });

    register_bridge_function(api, "__flushTimers__", [&self](choc::javascript::ArgumentList) {
        if (self.pending_timers_.empty()) return choc::value::Value();
        auto now = std::chrono::steady_clock::now();
        std::vector<int> to_fire;
        // Two-phase: first collect ids whose deadline has passed, then
        // fire them. Mutating pending_timers_ while iterating would lose
        // re-arms from setInterval (which reschedules its own deadline).
        for (auto& t : self.pending_timers_) {
            if (t.deadline <= now) to_fire.push_back(t.id);
        }
        if (to_fire.empty()) return choc::value::Value();
        // Re-arm intervals before invocation so the JS callback can call
        // clearInterval(id) and have it actually take effect (which the
        // __cancelTimer__ binding it calls into respects).
        for (auto& t : self.pending_timers_) {
            if (t.deadline <= now && t.repeating) {
                // Catch-up by interval - never schedule into the past.
                while (t.deadline <= now) t.deadline += t.interval;
            }
        }
        // Drop expired non-repeating timers from the queue. (Repeating
        // ones already had their deadline advanced above.)
        self.pending_timers_.erase(
            std::remove_if(self.pending_timers_.begin(), self.pending_timers_.end(),
                [now](const WidgetBridge::PendingTimer& p){
                    return !p.repeating && p.deadline <= now;
                }),
            self.pending_timers_.end());
        std::string batch;
        batch.reserve(to_fire.size() * 24);
        for (auto id : to_fire) {
            batch += "__invokeTimer__(";
            batch += std::to_string(id);
            batch += ");";
        }
        self.engine_.evaluate(batch + "void 0;");
        return choc::value::Value();
    });

    // performance.now() - high-resolution monotonic time in milliseconds.
    register_bridge_function(api, "__performanceNow__", [](choc::javascript::ArgumentList) {
        static auto start = std::chrono::steady_clock::now();
        auto now = std::chrono::steady_clock::now();
        double ms = std::chrono::duration<double, std::milli>(now - start).count();
        return choc::value::createFloat64(ms);
    });
}

} // namespace pulp::view
