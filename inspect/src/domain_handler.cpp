// domain_handler.cpp — Protocol request dispatch to inspector data sources

#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/state_inspector.hpp>
#include <pulp/inspect/console_capture.hpp>
#include <pulp/inspect/audio_inspector.hpp>
#include <pulp/inspect/motion_inspector.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/view.hpp>
#include <pulp/render/render_pass.hpp>

#include <choc/text/choc_JSON.h>

#include <sstream>
#include <iomanip>

namespace pulp::inspect {

using namespace pulp::view;

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
    if (domain == "Console")     return handle_console(req);
    if (domain == "Runtime")     return handle_runtime(req);
    if (domain == "Audio")       return handle_audio(req);
    if (domain == "Capture")     return handle_capture(req);
    if (domain == "Motion")      return handle_motion(req);

    return make_error(req.id, "Unknown domain: " + domain);
}

// ── Motion domain ───────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_motion(const InspectorMessage& req) {
    if (!motion_) return make_error(req.id, "No motion inspector attached");
    return motion_->handle(req);
}

// ── Inspector domain ────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_inspector(const InspectorMessage& req) {
    if (req.method == methods::kInspectorEnable) {
        if (overlay_) overlay_->set_active(true);
        return make_response(req.id, R"({"enabled":true})");
    }
    if (req.method == methods::kInspectorDisable) {
        if (overlay_) overlay_->set_active(false);
        return make_response(req.id, R"({"enabled":false})");
    }
    if (req.method == methods::kInspectorGetInfo) {
        auto obj = choc::value::createObject("");
        obj.addMember("framework", choc::value::createString("Pulp"));
        if (root_) {
            obj.addMember("view_count", choc::value::createInt64(
                static_cast<int64_t>(ViewInspector::count_views(*root_))));
        }
        if (overlay_) {
            obj.addMember("inspector_active", choc::value::createBool(overlay_->is_active()));
        }
        return make_response(req.id, choc::json::toString(obj, false));
    }
    return make_error(req.id, "Unknown Inspector method: " + req.method);
}

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
        // Highlighting is handled by the overlay — we'd need to find the view and set it
        // For now, return success (the CLI can highlight via the overlay)
        return make_response(req.id, R"({"ok":true})");
    }
    if (req.method == methods::kDOMClearHighlight) {
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

            auto passes = choc::value::createEmptyArray();
            for (auto& p : rpm_->passes()) {
                auto pass = choc::value::createObject("");
                pass.addMember("draw_calls", choc::value::createInt32(p.draw_calls));
                pass.addMember("time_ms", choc::value::createFloat64(p.time_ms));
                passes.addArrayElement(pass);
            }
            obj.addMember("passes", passes);
        } else {
            obj.addMember("available", choc::value::createBool(false));
        }
        return make_response(req.id, choc::json::toString(obj, false));
    }
    if (req.method == methods::kPerfEnableTracking) {
        // Tracking is always on when RenderPassManager exists
        return make_response(req.id, R"({"tracking":true})");
    }
    return make_error(req.id, "Unknown Performance method: " + req.method);
}

// ── State domain ────────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_state(const InspectorMessage& req) {
    if (!state_) return make_error(req.id, "No StateStore attached");

    if (req.method == methods::kStateGetParameters) {
        auto params = state_->all_params();
        auto arr = choc::value::createEmptyArray();
        for (auto& p : params) {
            auto obj = choc::value::createObject("");
            obj.addMember("id", choc::value::createInt32(static_cast<int32_t>(p.id)));
            obj.addMember("name", choc::value::createString(p.name));
            obj.addMember("unit", choc::value::createString(p.unit));
            obj.addMember("value", choc::value::createFloat64(p.value));
            obj.addMember("normalized", choc::value::createFloat64(p.normalized));
            obj.addMember("modulated", choc::value::createFloat64(p.modulated));
            obj.addMember("default", choc::value::createFloat64(p.default_value));
            obj.addMember("min", choc::value::createFloat64(p.min));
            obj.addMember("max", choc::value::createFloat64(p.max));
            if (!p.display_value.empty())
                obj.addMember("display", choc::value::createString(p.display_value));
            arr.addArrayElement(obj);
        }
        return make_response(req.id, choc::json::toString(arr, false));
    }
    if (req.method == methods::kStateSetParameter) {
        try {
            auto params = choc::json::parse(req.params_json);
            auto pid = static_cast<uint32_t>(params["id"].getInt64());
            auto value = static_cast<float>(params["value"].getFloat64());
            state_->set_param(pid, value);
            return make_response(req.id, R"({"ok":true})");
        } catch (...) {
            return make_error(req.id, "Invalid params for State.setParameter");
        }
    }
    return make_error(req.id, "Unknown State method: " + req.method);
}

// ── Console domain ──────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_console(const InspectorMessage& req) {
    if (req.method == methods::kConsoleEnable) {
        if (console_) {
            auto entries = console_->entries();
            auto arr = choc::value::createEmptyArray();
            for (auto& e : entries) {
                auto obj = choc::value::createObject("");
                obj.addMember("level", choc::value::createString(e.level));
                obj.addMember("message", choc::value::createString(e.message));
                arr.addArrayElement(obj);
            }
            return make_response(req.id, choc::json::toString(arr, false));
        }
        return make_response(req.id, "[]");
    }
    return make_error(req.id, "Unknown Console method: " + req.method);
}

// ── Runtime domain ──────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_runtime(const InspectorMessage& req) {
    if (req.method == methods::kRuntimeEvaluate) {
        // Would need a ScriptEngine reference — defer to future wiring
        return make_error(req.id, "Runtime.evaluate not yet wired (needs ScriptEngine reference)");
    }
    if (req.method == methods::kRuntimeGetHotReloadStatus) {
        return make_response(req.id, R"({"available":false})");
    }
    return make_error(req.id, "Unknown Runtime method: " + req.method);
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
    return make_error(req.id, "Unknown Audio method: " + req.method);
}

// ── Capture domain ──────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_capture(const InspectorMessage& req) {
    if (req.method == methods::kCaptureScreenshot) {
        // Would need WindowHost reference for capture_png()
        return make_error(req.id, "Capture.screenshot not yet wired (needs WindowHost reference)");
    }
    if (req.method == methods::kCaptureScreenshotNode) {
        return make_error(req.id, "Capture.screenshotNode not yet implemented");
    }
    return make_error(req.id, "Unknown Capture method: " + req.method);
}

} // namespace pulp::inspect
