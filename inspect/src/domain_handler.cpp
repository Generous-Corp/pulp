// domain_handler.cpp — Protocol request dispatch to inspector data sources

#include <pulp/inspect/domain_handler.hpp>

#include <pulp/state/param_json.hpp>
#include <pulp/view/value_channel_json.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/state_inspector.hpp>
#include <pulp/inspect/console_capture.hpp>
#include <pulp/inspect/audio_inspector.hpp>
#include <pulp/inspect/motion_inspector.hpp>
#include <pulp/inspect/motion_scrubber.hpp>
#include <pulp/inspect/trace_inspector.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/view.hpp>
#include <pulp/render/dirty_tracker.hpp>
#include <pulp/render/render_pass.hpp>
#include <pulp/view/live_constant_editor.hpp>

#include <choc/text/choc_JSON.h>

#include <sstream>
#include <iomanip>
#include <limits>
#include <stdexcept>

namespace pulp::inspect {

using namespace pulp::view;

// ── Config / wiring ─────────────────────────────────────────────────────────

void DomainHandler::set_trace_inspector(
    std::shared_ptr<TraceInspector> trace) {
    trace_ = trace.get();
    trace_binding_ = std::move(trace);
}

std::vector<InspectorPublicationBindingRegistration>
DomainHandler::publication_bindings() const {
    if (!trace_binding_)
        return {};
    return {{
        InspectorCapability::TraceSessionControl,
        trace_binding_,
    }};
}

void DomainHandler::set_config(InspectorConfig config) {
    config_ = std::move(config);
    // Keep the overlay's `J`-hotkey config in lockstep with the
    // protocol path so both resolve source-jumps identically.
    if (overlay_) overlay_->set_config(config_);
}

void DomainHandler::set_overlay(InspectorOverlay* overlay) {
    overlay_ = overlay;
    if (overlay_) overlay_->set_config(config_);
}

// ── Dispatch ────────────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle(const InspectorMessage& req) {
    auto dot = req.method.find('.');
    if (dot == std::string::npos)
        return make_error(req.id, "Invalid method: " + req.method);

    auto domain = req.method.substr(0, dot);

    if (domain == "Inspector")   return handle_inspector(req);
    if (domain == "DOM")         return handle_dom(req);
    if (domain == "CSS")         return handle_css(req);
    if (domain == "Performance") return handle_performance(req);
    if (domain == "State")       return handle_state(req);
    if (domain == "Test")        return handle_test(req);
    if (domain == "Console")     return handle_console(req);
    if (domain == "Runtime")     return handle_runtime(req);
    if (domain == "Audio")       return handle_audio(req);
    if (domain == "Capture")     return handle_capture(req);
    if (domain == "Motion")      return handle_motion(req);
    if (domain == "Trace")       return handle_trace(req);
    if (domain == "LiveConstant") return handle_live_constant(req);

    return make_error(req.id, "Unknown domain: " + domain);
}

InspectorMessage DomainHandler::handle_runtime_with_evaluator(
    const InspectorMessage& request, RuntimeEvaluator* evaluator) {
    auto* previous = runtime_evaluator_;
    runtime_evaluator_ = evaluator;
    struct RestoreBinding {
        RuntimeEvaluator*& slot;
        RuntimeEvaluator* previous;
        ~RestoreBinding() { slot = previous; }
    } restore{runtime_evaluator_, previous};
    return handle_runtime(request);
}

InspectorMessage DomainHandler::handle_test(const InspectorMessage& req) {
    return test_input_.handle(req);
}

// ── Motion domain ───────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_motion(const InspectorMessage& req) {
    // Scrubber methods route to MotionScrubber; everything else goes to
    // MotionInspector. Both data sources are optional, so a missing
    // scrubber returns a targeted error for scrubber methods rather
    // than masking them as unknown.
    if (MotionScrubber::owns_method(req.method)) {
        if (!motion_scrubber_)
            return make_error(req.id, "No motion scrubber attached");
        return motion_scrubber_->handle(req);
    }
    if (!motion_) return make_error(req.id, "No motion inspector attached");
    return motion_->handle(req);
}

InspectorMessage DomainHandler::handle_trace(const InspectorMessage& req) {
    if (!trace_) return make_error(req.id, "No trace inspector attached");
    return trace_->handle(req);
}

// ── Inspector domain ────────────────────────────────────────────────────────



// ── DOM domain ──────────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_dom(const InspectorMessage& req) {
    if (!root_) return make_error(req.id, "No root view attached");

    if (req.method == methods::kDOMGetDocument) {
        return make_response(req.id, ViewInspector::to_json(*root_));
    }
    if (req.method == methods::kDOMGetNodeById) {
        try {
            auto params = choc::json::parse(req.params_json);
            auto node_id = std::string(params["id"].getString());
            auto* found = ViewInspector::find_by_id(*root_, node_id);
            if (!found) return make_error(req.id, "View not found: " + node_id);
            // Return just this node's data (not recursive)
            auto obj = choc::value::createObject("");
            obj.addMember("type", choc::value::createString(ViewInspector::type_name(*found)));
            obj.addMember("id", choc::value::createString(found->id()));
            auto b = found->bounds();
            auto bounds = choc::value::createObject("");
            bounds.addMember("x", choc::value::createFloat64(b.x));
            bounds.addMember("y", choc::value::createFloat64(b.y));
            bounds.addMember("width", choc::value::createFloat64(b.width));
            bounds.addMember("height", choc::value::createFloat64(b.height));
            obj.addMember("bounds", bounds);
            obj.addMember("visible", choc::value::createBool(found->visible()));
            obj.addMember("opacity", choc::value::createFloat64(found->opacity()));
            obj.addMember("child_count", choc::value::createInt64(static_cast<int64_t>(found->child_count())));
            return make_response(req.id, choc::json::toString(obj, false));
        } catch (...) {
            return make_error(req.id, "Invalid params for DOM.getNodeById");
        }
    }
    if (req.method == methods::kDOMHighlightNode) {
        if (!overlay_)
            return make_error(req.id, "DOM.highlightNode: no overlay attached");
        try {
            const auto params = choc::json::parse(req.params_json);
            if (!params.isObject() || !params.hasObjectMember("id") ||
                !params["id"].isString())
                return make_error(req.id,
                                  "DOM.highlightNode requires string 'id'",
                                  "invalid_params");
            const auto node_id = std::string(params["id"].getString());
            if (node_id.empty() || node_id.size() > 256)
                return make_error(req.id,
                                  "DOM.highlightNode id must be 1..256 bytes",
                                  "invalid_params");
            auto* found = ViewInspector::find_by_id(*root_, node_id);
            if (!found)
                return make_error(req.id, "View not found: " + node_id,
                                  "invalid_params");
            overlay_->set_selected_view(found);
        } catch (...) {
            return make_error(req.id, "Invalid params for DOM.highlightNode",
                              "invalid_params");
        }
        return make_response(req.id, R"({"ok":true})");
    }
    if (req.method == methods::kDOMClearHighlight) {
        if (!overlay_)
            return make_error(req.id, "DOM.clearHighlight: no overlay attached");
        overlay_->set_selected_view(nullptr);
        return make_response(req.id, R"({"ok":true})");
    }
    if (req.method == methods::kDOMSearch) {
        try {
            auto params = choc::json::parse(req.params_json);
            auto query = std::string(params["query"].getString());
            // Simple search by id or type name
            auto results = choc::value::createEmptyArray();
            std::function<void(const View&)> search = [&](const View& v) {
                auto type = ViewInspector::type_name(v);
                auto vid = v.id();
                if (type.find(query) != std::string::npos || vid.find(query) != std::string::npos) {
                    auto entry = choc::value::createObject("");
                    entry.addMember("id", choc::value::createString(vid));
                    entry.addMember("type", choc::value::createString(type));
                    results.addArrayElement(entry);
                }
                for (size_t i = 0; i < v.child_count(); ++i)
                    search(*v.child_at(i));
            };
            search(*root_);
            return make_response(req.id, choc::json::toString(results, false));
        } catch (...) {
            return make_error(req.id, "Invalid params for DOM.search");
        }
    }
    return make_error(req.id, "Unknown DOM method: " + req.method);
}

// ── CSS domain ──────────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_css(const InspectorMessage& req) {
    if (!root_) return make_error(req.id, "No root view attached");

    if (req.method == methods::kCSSGetComputedStyle) {
        try {
            auto params = choc::json::parse(req.params_json);
            auto node_id = std::string(params["id"].getString());
            auto* found = ViewInspector::find_by_id(*root_, node_id);
            if (!found) return make_error(req.id, "View not found: " + node_id);

            auto obj = choc::value::createObject("");
            auto& f = found->flex();
            obj.addMember("direction", choc::value::createString(
                f.direction == FlexDirection::row ? "row" : "column"));
            obj.addMember("flex_grow", choc::value::createFloat64(f.flex_grow));
            obj.addMember("flex_shrink", choc::value::createFloat64(f.flex_shrink));
            obj.addMember("gap", choc::value::createFloat64(f.gap));
            obj.addMember("padding", choc::value::createFloat64(f.padding));
            obj.addMember("margin", choc::value::createFloat64(f.margin));
            obj.addMember("opacity", choc::value::createFloat64(found->opacity()));
            obj.addMember("visible", choc::value::createBool(found->visible()));

            // Theme colors
            auto colors = choc::value::createObject("");
            for (auto& [name, color] : found->theme().colors) {
                std::ostringstream hex;
                hex << "#" << std::hex << std::setfill('0')
                    << std::setw(2) << static_cast<int>(color.r * 255)
                    << std::setw(2) << static_cast<int>(color.g * 255)
                    << std::setw(2) << static_cast<int>(color.b * 255);
                colors.addMember(name, choc::value::createString(hex.str()));
            }
            obj.addMember("theme_colors", colors);

            return make_response(req.id, choc::json::toString(obj, false));
        } catch (...) {
            return make_error(req.id, "Invalid params for CSS.getComputedStyle");
        }
    }
    if (req.method == methods::kCSSGetTheme) {
        if (root_) {
            return make_response(req.id, root_->theme().to_json());
        }
        return make_error(req.id, "No root view");
    }
    return make_error(req.id, "Unknown CSS method: " + req.method);
}

// ── Performance domain ──────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_performance(const InspectorMessage& req) {
    if (req.method == methods::kPerfGetMetrics) {
        auto obj = choc::value::createObject("");
        if (rpm_) {
            obj.addMember("total_time_ms", choc::value::createFloat64(rpm_->total_time_ms()));
            obj.addMember("frame_count", choc::value::createInt64(static_cast<int64_t>(rpm_->frame_count())));
            obj.addMember("budget_ms", choc::value::createFloat64(rpm_->budget()));
            obj.addMember("over_budget", choc::value::createBool(rpm_->over_budget()));

            // Per-pass timing reports CPU wall-time only.
            //  - `time_ms`/`cpu_time_ms`: CPU wall-time around draw
            //    submission (legacy `time_ms` kept for back-compat with
            //    overlay consumers).
            // There is deliberately NO per-pass GPU number: Skia Graphite owns
            // the command encoders, so Pulp cannot inject per-pass timestamp
            // writes from outside. Per-pass GPU timing is structurally
            // unavailable under Graphite; the honest GPU clock is the
            // frame-level whole-recording number below.
            auto passes = choc::value::createEmptyArray();
            for (auto& p : rpm_->passes()) {
                auto pass = choc::value::createObject("");
                pass.addMember("draw_calls", choc::value::createInt32(p.draw_calls));
                pass.addMember("time_ms", choc::value::createFloat64(p.time_ms));
                pass.addMember("cpu_time_ms", choc::value::createFloat64(p.cpu_time_ms()));
                passes.addArrayElement(pass);
            }
            obj.addMember("passes", passes);

            // Frame-level, whole-recording GPU *render* time (Skia
            // Graphite GpuStats elapsed time). This is the only GPU-clock
            // granularity the Graphite path exposes.
            // `gpu_render_timing_available` gates it honestly.
            obj.addMember("gpu_render_time_ms",
                          choc::value::createFloat64(rpm_->gpu_render_time_ms()));
            obj.addMember("gpu_render_timing_available",
                          choc::value::createBool(rpm_->gpu_render_timing_available()));
        } else {
            obj.addMember("available", choc::value::createBool(false));
        }
        return make_response(req.id, choc::json::toString(obj, false));
    }
    if (req.method == methods::kPerfEnableTracking) {
        // Tracking is always on when RenderPassManager exists
        return make_response(req.id, R"({"tracking":true})");
    }
    // Per-repaint "flash" overlay. Wraps DirtyTracker::set_debug_overlay().
    // When no tracker is wired we report the toggle as unavailable so the
    // UI can grey it out instead of silently dropping clicks.
    if (req.method == methods::kPerfGetRepaintFlash) {
        auto obj = choc::value::createObject("");
        if (dirty_) {
            obj.addMember("available", choc::value::createBool(true));
            obj.addMember("enabled",
                          choc::value::createBool(dirty_->debug_overlay()));
        } else {
            obj.addMember("available", choc::value::createBool(false));
            obj.addMember("enabled", choc::value::createBool(false));
        }
        return make_response(req.id, choc::json::toString(obj, false));
    }
    if (req.method == methods::kPerfSetRepaintFlash) {
        if (!dirty_) {
            return make_error(req.id,
                "Performance.setRepaintFlash: no DirtyTracker attached");
        }
        // Parse {"enabled": true|false}. Default to true if absent so a
        // bare invocation enables (the most common case from a UI toggle).
        bool enabled = true;
        try {
            auto v = choc::json::parse(req.params_json);
            if (v.isObject() && v.hasObjectMember("enabled")) {
                enabled = v["enabled"].getBool();
            }
        } catch (...) {
            // Malformed JSON: keep default (enabled = true).
        }
        dirty_->set_debug_overlay(enabled);
        auto obj = choc::value::createObject("");
        obj.addMember("enabled", choc::value::createBool(enabled));
        return make_response(req.id, choc::json::toString(obj, false));
    }
    return make_error(req.id, "Unknown Performance method: " + req.method);
}

// ── State domain ────────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_state(const InspectorMessage& req) {
    if (!state_) return make_error(req.id, "No StateStore attached");

    if (req.method == methods::kStateGetParameters) {
        // Serialized by pulp::state::param_json, the same code the scripted-UI
        // bridge uses. One implementation is what keeps the two payloads
        // identical; a second hand-rolled object here would drift the day
        // either side gained a field.
        auto& store = state_->store();
        auto arr = choc::value::createEmptyArray();
        for (std::size_t i = 0; i < store.param_count(); ++i)
            arr.addArrayElement(state::param_snapshot_to_value(store, store.all_params()[i]));
        return make_response(req.id, choc::json::toString(arr, false));
    }
    if (req.method == methods::kStateGetValueChannels) {
        // What the processor publishes that is NOT a parameter — gain
        // reduction, an envelope, a spectrum. Serialized by the same
        // value_channel_json the scripted-UI bridge's listValueChannels() uses,
        // so the two descriptions of one channel set cannot drift.
        return make_response(
            req.id,
            choc::json::toString(view::value_channels_to_value(state_->value_channels()), false));
    }
    if (req.method == methods::kStateSetParameter) {
        try {
            auto params = choc::json::parse(req.params_json);
            const auto& id_json = params["id"];
            if (!id_json.isInt())
                throw std::runtime_error("State.setParameter id is not an integer");
            const auto raw_pid = id_json.getInt64();
            if (raw_pid < 0 ||
                static_cast<std::uint64_t>(raw_pid) >
                    std::numeric_limits<std::uint32_t>::max())
                throw std::runtime_error("State.setParameter id is out of range");
            const auto pid = static_cast<std::uint32_t>(raw_pid);
            const auto& value_json = params["value"];
            if (!value_json.isInt() && !value_json.isFloat())
                throw std::runtime_error("State.setParameter value is not numeric");
            auto value = static_cast<float>(value_json.getWithDefault(0.0));
            // Optional: interpret value as a 0..1 normalized position.
            bool normalized = params.isObject() && params.hasObjectMember("normalized")
                                  ? params["normalized"].getWithDefault(false)
                                  : false;
            if (!state_->set_param(pid, value, normalized))
                return make_error(req.id, "Unknown parameter id: " + std::to_string(pid));
            return make_response(req.id, R"({"ok":true})");
        } catch (...) {
            return make_error(req.id, "Invalid params for State.setParameter");
        }
    }
    return make_error(req.id, "Unknown State method: " + req.method);
}

// ── Audio domain ────────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_audio(const InspectorMessage& req) {
    if (!audio_) return make_error(req.id, "No AudioInspector attached");

    if (req.method == methods::kAudioGetConfig) {
        auto cfg = audio_->config();
        auto obj = choc::value::createObject("");
        obj.addMember("sample_rate", choc::value::createFloat64(cfg.sample_rate));
        obj.addMember("buffer_size", choc::value::createInt32(cfg.buffer_size));
        obj.addMember("input_channels", choc::value::createInt32(cfg.input_channels));
        obj.addMember("output_channels", choc::value::createInt32(cfg.output_channels));
        obj.addMember("latency_samples", choc::value::createInt32(cfg.latency_samples));
        return make_response(req.id, choc::json::toString(obj, false));
    }
    if (req.method == methods::kAudioEnableMetering) {
        audio_->set_metering_enabled(true);
        return make_response(req.id, R"({"metering":true})");
    }
    if (req.method == methods::kAudioGetMidiLog) {
        auto events = audio_->recent_midi();
        auto arr = choc::value::createEmptyArray();
        for (auto& e : events) {
            auto obj = choc::value::createObject("");
            obj.addMember("status", choc::value::createInt32(e.status));
            obj.addMember("data1", choc::value::createInt32(e.data1));
            obj.addMember("data2", choc::value::createInt32(e.data2));
            if (!e.description.empty())
                obj.addMember("description", choc::value::createString(e.description));
            arr.addArrayElement(obj);
        }
        return make_response(req.id, choc::json::toString(arr, false));
    }
    if (req.method == methods::kAudioRuntimeTelemetry) {
        const auto telemetry = audio_->runtime_telemetry();
        auto obj = choc::value::createObject("");
        obj.addMember("available", choc::value::createBool(telemetry.available));
        obj.addMember("xrun_count", choc::value::createInt64(
            static_cast<int64_t>(telemetry.xrun_count)));

        auto load = choc::value::createObject("");
        load.addMember("load", choc::value::createFloat64(telemetry.process_load.load));
        load.addMember("peak_load",
                       choc::value::createFloat64(telemetry.process_load.peak_load));
        load.addMember("last_load",
                       choc::value::createFloat64(telemetry.process_load.last_load));
        load.addMember("elapsed_ns", choc::value::createInt64(
            telemetry.process_load.elapsed_ns));
        load.addMember("available_ns", choc::value::createInt64(
            telemetry.process_load.available_ns));
        load.addMember("callback_count", choc::value::createInt64(
            static_cast<int64_t>(telemetry.process_load.callback_count)));
        load.addMember("overload_count", choc::value::createInt64(
            static_cast<int64_t>(telemetry.process_load.overload_count)));
        obj.addMember("process_load", load);

        return make_response(req.id, choc::json::toString(obj, false));
    }
    return make_error(req.id, "Unknown Audio method: " + req.method);
}



// ── LiveConstant domain ───────────────────────────────────────────────────
//
// Wires PULP_LIVE_CONSTANT(name, default, min, max) to the inspector
// via three RPC methods. No host setter is required — the registry
// is a static singleton, so the inspector reaches it directly.

InspectorMessage DomainHandler::handle_live_constant(const InspectorMessage& req) {
    auto& registry = pulp::view::LiveConstantRegistry::instance();

    if (req.method == methods::kLiveConstList) {
        auto arr = choc::value::createEmptyArray();
        for (const auto& c : registry.all()) {
            auto obj = choc::value::createObject("");
            obj.addMember("name", choc::value::createString(c.name));
            obj.addMember("file", choc::value::createString(c.file));
            obj.addMember("line", choc::value::createInt32(c.line));
            obj.addMember("value", choc::value::createFloat32(c.value));
            obj.addMember("default", choc::value::createFloat32(c.default_value));
            obj.addMember("min", choc::value::createFloat32(c.min_value));
            obj.addMember("max", choc::value::createFloat32(c.max_value));
            arr.addArrayElement(obj);
        }
        auto out = choc::value::createObject("");
        out.addMember("constants", arr);
        return make_response(req.id, choc::json::toString(out, false));
    }
    if (req.method == methods::kLiveConstSet) {
        std::string name;
        float value = 0.0f;
        try {
            auto v = choc::json::parse(req.params_json);
            if (v.isObject()) {
                if (v.hasObjectMember("name")) name = v["name"].getString();
                if (v.hasObjectMember("value")) {
                    // JSON numbers parse to choc int / float / double
                    // depending on literal shape; coerce via Float64 which
                    // accepts any numeric subtype, then narrow.
                    value = static_cast<float>(
                        v["value"].getWithDefault(0.0));
                }
            }
        } catch (...) {}
        if (name.empty()) {
            return make_error(req.id,
                "LiveConstant.set: missing or invalid 'name' field");
        }
        registry.set(name, value);
        auto out = choc::value::createObject("");
        out.addMember("name", choc::value::createString(name));
        out.addMember("value", choc::value::createFloat32(value));
        return make_response(req.id, choc::json::toString(out, false));
    }
    if (req.method == methods::kLiveConstReset) {
        std::string name;
        try {
            auto v = choc::json::parse(req.params_json);
            if (v.isObject() && v.hasObjectMember("name"))
                name = v["name"].getString();
        } catch (...) {}
        if (name.empty()) {
            registry.reset_all();
        } else {
            registry.reset(name);
        }
        return make_response(req.id, R"({"ok":true})");
    }
    return make_error(req.id, "Unknown LiveConstant method: " + req.method);
}

} // namespace pulp::inspect
