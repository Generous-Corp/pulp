// domain_handler.cpp — Protocol request dispatch to inspector data sources

#include <pulp/inspect/domain_handler.hpp>

#include <pulp/state/param_json.hpp>
#include <pulp/view/value_channel_json.hpp>
#include <pulp/inspect/editor_url.hpp>
#include <pulp/inspect/source_jump.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/state_inspector.hpp>
#include <pulp/inspect/console_capture.hpp>
#include <pulp/inspect/audio_inspector.hpp>
#include <pulp/inspect/motion_inspector.hpp>
#include <pulp/inspect/motion_scrubber.hpp>
#include <pulp/inspect/trace_inspector.hpp>
#include <pulp/inspect/tweak_store.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/view.hpp>
#include <pulp/render/dirty_tracker.hpp>
#include <pulp/render/render_pass.hpp>
#include <pulp/runtime/base64.hpp>
#include <pulp/view/live_constant_editor.hpp>

#include <choc/text/choc_JSON.h>

#include <sstream>
#include <iomanip>
#include <limits>
#include <stdexcept>
#include <unordered_set>

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
        if (tweak_store_) {
            obj.addMember("tweak_count", choc::value::createInt64(
                static_cast<int64_t>(tweak_store_->count())));
        }
        return make_response(req.id, choc::json::toString(obj, false));
    }
    if (req.method == methods::kInspectorGetAgentContext) {
        if (!agent_context_)
            return make_error(req.id, "No agent context source attached",
                              "context_unavailable");
        const auto context = agent_context_->snapshot();
        auto obj = choc::value::createObject("");
        auto binary = choc::value::createObject("");
        binary.addMember("path", choc::value::createString(context.binary_path));
        binary.addMember("buildId", choc::value::createString(context.binary_build_id));
        binary.addMember("mtimeUnixMs",
                         choc::value::createInt64(context.binary_mtime_unix_ms));
        obj.addMember("binary", binary);
        auto identity = choc::value::createObject("");
        identity.addMember("pluginId", choc::value::createString(context.plugin_id));
        identity.addMember("sessionId", choc::value::createString(context.session_id));
        identity.addMember("instanceId", choc::value::createString(context.instance_id));
        obj.addMember("identity", identity);
        auto editor = choc::value::createObject("");
        editor.addMember("open", choc::value::createBool(context.editor_open));
        editor.addMember("windowVisible", choc::value::createBool(context.window_visible));
        obj.addMember("editor", editor);
        auto processing = choc::value::createObject("");
        processing.addMember("active", choc::value::createBool(context.processing));
        processing.addMember("xrunCount", choc::value::createInt64(
            static_cast<std::int64_t>(context.xrun_count)));
        obj.addMember("processing", processing);
        auto reload = choc::value::createObject("");
        reload.addMember("available", choc::value::createBool(context.hot_reload_available));
        reload.addMember("enabled", choc::value::createBool(context.hot_reload_enabled));
        reload.addMember("pending", choc::value::createBool(context.hot_reload_pending));
        obj.addMember("hotReload", reload);
        obj.addMember("unsavedTweakCount", choc::value::createInt64(
            static_cast<std::int64_t>(context.unsaved_tweak_count)));
        auto issues = choc::value::createEmptyArray();
        for (const auto& issue : context.actionable_issues)
            issues.addArrayElement(choc::value::createString(issue));
        obj.addMember("actionableIssues", issues);
        return make_response(req.id, choc::json::toString(obj, false));
    }

    // ── Tweak storage RPCs ─────────────────────────────────────────
    // Tweak RPCs require a TweakStore wired in (set_tweak_store(...)).
    // Schema mirrors the TS @pulp/import-ir/src/tweaks.ts TweaksFile.
    if (req.method == methods::kInspectorApplyTweak) {
        if (!tweak_store_) return make_error(req.id, "No tweak store attached");
        try {
            auto params = choc::json::parse(req.params_json);
            auto anchor = std::string(params["anchorId"].getString());
            auto path = std::string(params["propertyPath"].getString());
            std::string source;
            if (params.hasObjectMember("source") && params["source"].isString())
                source = std::string(params["source"].getString());
            // `value` is arbitrary JSON — clone it into a value the
            // store can own. (params is a ValueView over a temporary
            // parsed-JSON document; addMember copies cleanly.)
            auto value = choc::value::Value(params["value"]);
            auto total = tweak_store_->apply_tweak(anchor, path, std::move(value), source);

            auto resp = choc::value::createObject("");
            resp.addMember("ok", choc::value::createBool(true));
            resp.addMember("tweakCount", choc::value::createInt64(static_cast<int64_t>(total)));
            return make_response(req.id, choc::json::toString(resp, false));
        } catch (const std::exception& e) {
            return make_error(req.id,
                std::string("Invalid params for Inspector.applyTweak: ") + e.what());
        } catch (...) {
            return make_error(req.id, "Invalid params for Inspector.applyTweak");
        }
    }
    if (req.method == methods::kInspectorListTweaks) {
        if (!tweak_store_) return make_error(req.id, "No tweak store attached");
        auto records = tweak_store_->list_tweaks();

        // Build the response as { tweaks: { anchor: { path: value } },
        // bypassed: { anchor: true | [paths] } } — mirrors the on-disk
        // TweaksFile schema.
        auto tweaks_obj = choc::value::createObject("");
        std::unordered_map<std::string, choc::value::Value> anchor_objs;
        for (auto& rec : records) {
            auto it = anchor_objs.find(rec.anchor_id);
            if (it == anchor_objs.end()) {
                it = anchor_objs.emplace(rec.anchor_id,
                                         choc::value::createObject("")).first;
            }
            it->second.addMember(rec.property_path, rec.value);
        }
        for (auto& [anchor, obj] : anchor_objs) {
            tweaks_obj.addMember(anchor, obj);
        }

        auto bypassed_obj = choc::value::createObject("");
        // Include bypass-only anchors. A setBypass call on an anchor
        // that had no active tweaks, or whose tweaks were later cleared
        // via Inspector.clearTweaks without clearing the bypass, must
        // still surface in the response. Walk both the tweak-record
        // anchors and the TweakStore's bypassed anchors so the protocol
        // can round-trip every bypass state.
        std::unordered_set<std::string> all_anchors;
        for (auto& rec : records) all_anchors.insert(rec.anchor_id);
        for (auto& anchor : tweak_store_->bypassed_anchors())
            all_anchors.insert(std::move(anchor));
        for (auto& anchor : all_anchors) {
            auto b = tweak_store_->bypass_for(anchor);
            if (!b) continue;
            std::visit([&](auto&& v) {
                using T = std::decay_t<decltype(v)>;
                if constexpr (std::is_same_v<T, bool>) {
                    bypassed_obj.addMember(anchor, choc::value::createBool(v));
                } else {
                    auto arr = choc::value::createEmptyArray();
                    for (auto& p : v) arr.addArrayElement(choc::value::createString(p));
                    bypassed_obj.addMember(anchor, arr);
                }
            }, *b);
        }

        // Surface the `locked` overlay so the management panel and
        // disk-persistence path can round-trip lock state.
        auto locked_arr = choc::value::createEmptyArray();
        for (auto& anchor : tweak_store_->locked_anchors())
            locked_arr.addArrayElement(choc::value::createString(anchor));

        auto resp = choc::value::createObject("");
        resp.addMember("tweaks", tweaks_obj);
        resp.addMember("bypassed", bypassed_obj);
        resp.addMember("locked", locked_arr);
        resp.addMember("count", choc::value::createInt64(static_cast<int64_t>(records.size())));
        return make_response(req.id, choc::json::toString(resp, false));
    }
    if (req.method == methods::kInspectorClearTweaks) {
        if (!tweak_store_) return make_error(req.id, "No tweak store attached");
        try {
            auto params = req.params_json.empty() || req.params_json == "{}"
                ? choc::value::createObject("")
                : choc::json::parse(req.params_json);
            std::size_t removed = 0;
            bool has_anchor = params.isObject() &&
                              params.hasObjectMember("anchorId") &&
                              params["anchorId"].isString();
            bool has_path = params.isObject() &&
                            params.hasObjectMember("propertyPath") &&
                            params["propertyPath"].isString();
            if (has_anchor && has_path) {
                removed = tweak_store_->remove_tweak(
                    std::string(params["anchorId"].getString()),
                    std::string(params["propertyPath"].getString())) ? 1 : 0;
            } else if (has_anchor) {
                removed = tweak_store_->remove_anchor(
                    std::string(params["anchorId"].getString()));
            } else {
                // No selector — wipe the whole table.
                removed = tweak_store_->count();
                tweak_store_->clear();
            }
            auto resp = choc::value::createObject("");
            resp.addMember("ok", choc::value::createBool(true));
            resp.addMember("removed", choc::value::createInt64(static_cast<int64_t>(removed)));
            return make_response(req.id, choc::json::toString(resp, false));
        } catch (...) {
            return make_error(req.id, "Invalid params for Inspector.clearTweaks");
        }
    }
    // ── Tweak disk persistence RPCs ────────────────────────────────
    if (req.method == methods::kInspectorLoadTweaks) {
        if (!tweak_store_) return make_error(req.id, "No tweak store attached");
        std::string path;
        try {
            if (!req.params_json.empty() && req.params_json != "{}") {
                auto params = choc::json::parse(req.params_json);
                if (params.isObject() && params.hasObjectMember("path") &&
                    params["path"].isString()) {
                    path = std::string(params["path"].getString());
                }
            }
        } catch (...) {
            return make_error(req.id, "Invalid params for Inspector.loadTweaks");
        }
        auto r = tweak_store_->load_from_disk(path);
        if (!r.ok) {
            return make_error(req.id,
                std::string("Inspector.loadTweaks failed: ") + r.error);
        }
        auto resp = choc::value::createObject("");
        resp.addMember("ok", choc::value::createBool(true));
        resp.addMember("path", choc::value::createString(r.path));
        resp.addMember("tweakCount",
            choc::value::createInt64(static_cast<int64_t>(r.tweak_count)));
        resp.addMember("bypassCount",
            choc::value::createInt64(static_cast<int64_t>(r.bypass_count)));
        return make_response(req.id, choc::json::toString(resp, false));
    }
    if (req.method == methods::kInspectorSaveTweaks) {
        if (!tweak_store_) return make_error(req.id, "No tweak store attached");
        std::string path;
        try {
            if (!req.params_json.empty() && req.params_json != "{}") {
                auto params = choc::json::parse(req.params_json);
                if (params.isObject() && params.hasObjectMember("path") &&
                    params["path"].isString()) {
                    path = std::string(params["path"].getString());
                }
            }
        } catch (...) {
            return make_error(req.id, "Invalid params for Inspector.saveTweaks");
        }
        auto r = tweak_store_->save_to_disk(path);
        if (!r.ok) {
            return make_error(req.id,
                std::string("Inspector.saveTweaks failed: ") + r.error);
        }
        auto resp = choc::value::createObject("");
        resp.addMember("ok", choc::value::createBool(true));
        resp.addMember("path", choc::value::createString(r.path));
        resp.addMember("tweakCount",
            choc::value::createInt64(static_cast<int64_t>(r.tweak_count)));
        resp.addMember("bypassCount",
            choc::value::createInt64(static_cast<int64_t>(r.bypass_count)));
        return make_response(req.id, choc::json::toString(resp, false));
    }
    if (req.method == methods::kInspectorSetAutoSave) {
        if (!tweak_store_) return make_error(req.id, "No tweak store attached");
        bool enabled = false;
        std::string path;
        try {
            auto params = choc::json::parse(req.params_json);
            if (!params.isObject() || !params.hasObjectMember("enabled") ||
                !params["enabled"].isBool()) {
                return make_error(req.id,
                    "Inspector.setAutoSave requires `enabled` as bool");
            }
            enabled = params["enabled"].getBool();
            if (params.hasObjectMember("path") && params["path"].isString()) {
                path = std::string(params["path"].getString());
            }
        } catch (...) {
            return make_error(req.id, "Invalid params for Inspector.setAutoSave");
        }
        tweak_store_->set_auto_save(enabled, path);
        auto resp = choc::value::createObject("");
        resp.addMember("ok", choc::value::createBool(true));
        resp.addMember("enabled", choc::value::createBool(enabled));
        resp.addMember("path", choc::value::createString(tweak_store_->auto_save_path()));
        return make_response(req.id, choc::json::toString(resp, false));
    }

    // ── Editor URL template RPCs ───────────────────────────────────
    // setEditorUrlTemplate validates and stores; getEditorUrlTemplate
    // reports the effective template and where it came from
    // (env / config / default). Actual jumping is handled by
    // Inspector.jumpToSource below.
    if (req.method == methods::kInspectorSetEditorUrlTemplate) {
        try {
            auto params = choc::json::parse(req.params_json);
            if (!params.isObject() ||
                !params.hasObjectMember("template") ||
                !params["template"].isString()) {
                return make_error(req.id,
                    "Inspector.setEditorUrlTemplate requires `template` as string");
            }
            std::string tmpl = std::string(params["template"].getString());
            std::string err;
            if (!validate_editor_url_template(tmpl, &err))
                return make_error(req.id,
                    std::string("Inspector.setEditorUrlTemplate: ") + err);
            config_.editor_url_template = tmpl;
            // Keep the overlay's `J`-hotkey config in sync so a runtime
            // template change reaches both jump paths.
            if (overlay_) overlay_->set_config(config_);
            auto resp = choc::value::createObject("");
            resp.addMember("ok", choc::value::createBool(true));
            resp.addMember("template", choc::value::createString(tmpl));
            return make_response(req.id, choc::json::toString(resp, false));
        } catch (const std::exception& e) {
            return make_error(req.id,
                std::string("Invalid params for Inspector.setEditorUrlTemplate: ") + e.what());
        } catch (...) {
            return make_error(req.id, "Invalid params for Inspector.setEditorUrlTemplate");
        }
    }
    if (req.method == methods::kInspectorGetEditorUrlTemplate) {
        auto eff = effective_editor_url(config_);
        auto resp = choc::value::createObject("");
        resp.addMember("template", choc::value::createString(eff.template_str));
        resp.addMember("source",
            choc::value::createString(std::string(editor_url_source_name(eff.source))));
        resp.addMember("configTemplate",
            choc::value::createString(config_.editor_url_template));
        if (auto env = editor_url_env_override())
            resp.addMember("envTemplate", choc::value::createString(*env));
        return make_response(req.id, choc::json::toString(resp, false));
    }

    // ── Inspector.jumpToSource ─────────────────────────────────────
    // Resolve a view's authored source location and open the user's
    // editor at file:line. Params (all optional):
    //   anchorId : design-import anchor of the target view. When
    //              omitted, falls back to the overlay's selected view.
    //   dryRun   : when true, resolve + format only — never spawn a
    //              process. Defaults to true so tests and agent callers
    //              must opt into a real launch with dryRun:false.
    // Returns {ok, url, path, line, col, launched} on a resolvable
    // target; {ok:false, error} when the view has no provenance or no
    // target could be found. Note `ok:false` here is a structured
    // response, NOT a protocol error — a no-provenance view is a
    // normal, expected case (the overlay treats it as a graceful
    // no-op), so callers can branch on `ok` without exception handling.
    if (req.method == methods::kInspectorJumpToSource) {
        bool dry_run = true;
        std::string anchor_id;
        if (!req.params_json.empty()) {
            try {
                auto params = choc::json::parse(req.params_json);
                if (params.isObject()) {
                    if (params.hasObjectMember("dryRun") &&
                        params["dryRun"].isBool())
                        dry_run = params["dryRun"].getBool();
                    if (params.hasObjectMember("anchorId") &&
                        params["anchorId"].isString())
                        anchor_id = std::string(params["anchorId"].getString());
                }
            } catch (...) {
                return make_error(req.id,
                    "Invalid params for Inspector.jumpToSource");
            }
        }

        // Resolve the target view: explicit anchor wins; otherwise the
        // overlay's current selection.
        view::View* target = nullptr;
        if (!anchor_id.empty()) {
            if (root_)
                target = ViewInspector::find_by_anchor(*root_, anchor_id);
        } else if (overlay_) {
            target = overlay_->selected_view();
        }

        auto result = jump_to_source(config_, target, dry_run);

        auto resp = choc::value::createObject("");
        resp.addMember("ok", choc::value::createBool(result.ok));
        if (result.ok) {
            resp.addMember("url", choc::value::createString(result.url));
            resp.addMember("path", choc::value::createString(result.path));
            resp.addMember("line",
                choc::value::createInt64(static_cast<int64_t>(result.line)));
            resp.addMember("col",
                choc::value::createInt64(static_cast<int64_t>(result.col)));
            resp.addMember("launched", choc::value::createBool(result.launched));
            resp.addMember("dryRun", choc::value::createBool(dry_run));
            if (!result.error.empty())
                resp.addMember("launchError", choc::value::createString(result.error));
        } else {
            resp.addMember("error", choc::value::createString(
                anchor_id.empty() && !overlay_
                    ? std::string("no overlay attached and no anchorId given")
                    : result.error));
        }
        return make_response(req.id, choc::json::toString(resp, false));
    }
    if (req.method == methods::kInspectorSetBypass) {
        if (!tweak_store_) return make_error(req.id, "No tweak store attached");
        try {
            auto params = choc::json::parse(req.params_json);
            auto anchor = std::string(params["anchorId"].getString());
            // Value can be `true`/`false` (whole-anchor) or an array of
            // dotted paths (path-scoped). Empty array / false clears.
            if (params.hasObjectMember("value") && params["value"].isBool()) {
                tweak_store_->set_bypass(anchor, params["value"].getBool());
            } else if (params.hasObjectMember("value") && params["value"].isArray()) {
                std::vector<std::string> paths;
                auto arr = params["value"];
                for (uint32_t i = 0; i < arr.size(); ++i) {
                    if (arr[i].isString()) paths.push_back(std::string(arr[i].getString()));
                }
                tweak_store_->set_bypass(anchor, std::move(paths));
            } else {
                return make_error(req.id,
                    "Inspector.setBypass requires `value` as bool or string[]");
            }
            auto resp = choc::value::createObject("");
            resp.addMember("ok", choc::value::createBool(true));
            return make_response(req.id, choc::json::toString(resp, false));
        } catch (...) {
            return make_error(req.id, "Invalid params for Inspector.setBypass");
        }
    }
    // ── setLocked mirrors setBypass for the lock overlay ───────────
    if (req.method == methods::kInspectorSetLocked) {
        if (!tweak_store_) return make_error(req.id, "No tweak store attached");
        try {
            auto params = choc::json::parse(req.params_json);
            auto anchor = std::string(params["anchorId"].getString());
            if (!params.hasObjectMember("value") || !params["value"].isBool()) {
                return make_error(req.id,
                    "Inspector.setLocked requires `value` as bool");
            }
            tweak_store_->set_locked(anchor, params["value"].getBool());
            auto resp = choc::value::createObject("");
            resp.addMember("ok", choc::value::createBool(true));
            resp.addMember("locked", choc::value::createBool(
                tweak_store_->is_locked(anchor)));
            return make_response(req.id, choc::json::toString(resp, false));
        } catch (...) {
            return make_error(req.id, "Invalid params for Inspector.setLocked");
        }
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
            const auto id = id_json.getInt64();
            if (id < 0 || id > std::numeric_limits<std::uint32_t>::max())
                throw std::runtime_error("State.setParameter id is outside uint32 range");
            const auto pid = static_cast<std::uint32_t>(id);
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

// ── Capture domain ──────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_capture(const InspectorMessage& req) {
    if (req.method == methods::kCaptureScreenshot) {
        if (!capture_)
            return make_error(req.id, "No capture source attached",
                              "capture_unavailable");
        auto captured = capture_->capture_png();
        if (!captured.error.empty())
            return make_error(req.id, captured.error, "capture_failed");
        if (captured.png.empty())
            return make_error(req.id, "Capture source returned no PNG bytes",
                              "capture_failed");
        auto obj = choc::value::createObject("");
        obj.addMember("mimeType", choc::value::createString("image/png"));
        obj.addMember("width", choc::value::createInt64(captured.width));
        obj.addMember("height", choc::value::createInt64(captured.height));
        obj.addMember("data", choc::value::createString(
            runtime::base64_encode(captured.png.data(), captured.png.size())));
        return make_response(req.id, choc::json::toString(obj, false));
    }
    if (req.method == methods::kCaptureScreenshotNode) {
        return make_error(req.id, "Capture.screenshotNode is unavailable",
                          "method_unavailable");
    }
    return make_error(req.id, "Unknown Capture method: " + req.method);
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
