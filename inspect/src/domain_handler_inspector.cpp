// Inspector-domain protocol methods and authoring state.

#include <pulp/inspect/domain_handler.hpp>

#include <pulp/inspect/editor_url.hpp>
#include <pulp/inspect/inspector_overlay.hpp>
#include <pulp/inspect/source_jump.hpp>
#include <pulp/inspect/tweak_store.hpp>
#include <pulp/view/inspector.hpp>
#include <pulp/view/view.hpp>

#include <choc/text/choc_JSON.h>

#include <exception>
#include <string>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <variant>
#include <vector>

namespace pulp::inspect {

using namespace pulp::view;

InspectorMessage DomainHandler::handle_inspector(const InspectorMessage& req) {
    if (req.method == methods::kInspectorEnable) {
        if (overlay_)
            overlay_->set_active(true);
        return make_response(req.id, R"({"enabled":true})");
    }
    if (req.method == methods::kInspectorDisable) {
        if (overlay_)
            overlay_->set_active(false);
        return make_response(req.id, R"({"enabled":false})");
    }
    if (req.method == methods::kInspectorGetInfo) {
        auto obj = choc::value::createObject("");
        obj.addMember("framework", choc::value::createString("Pulp"));
        if (root_) {
            obj.addMember("view_count", choc::value::createInt64(static_cast<int64_t>(
                                            ViewInspector::count_views(*root_))));
        }
        if (overlay_) {
            obj.addMember("inspector_active", choc::value::createBool(overlay_->is_active()));
        }
        if (tweak_store_) {
            obj.addMember("tweak_count",
                          choc::value::createInt64(static_cast<int64_t>(tweak_store_->count())));
        }
        return make_response(req.id, choc::json::toString(obj, false));
    }
    if (req.method == methods::kInspectorGetAgentContext) {
        if (!agent_context_)
            return make_error(req.id, "No agent context source attached", "context_unavailable");
        const auto context = agent_context_->snapshot();
        auto obj = choc::value::createObject("");
        auto binary = choc::value::createObject("");
        binary.addMember("path", choc::value::createString(context.binary_path));
        binary.addMember("buildId", choc::value::createString(context.binary_build_id));
        binary.addMember("mtimeUnixMs", choc::value::createInt64(context.binary_mtime_unix_ms));
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
        processing.addMember(
            "xrunCount", choc::value::createInt64(static_cast<std::int64_t>(context.xrun_count)));
        obj.addMember("processing", processing);
        auto reload = choc::value::createObject("");
        reload.addMember("available", choc::value::createBool(context.hot_reload_available));
        reload.addMember("enabled", choc::value::createBool(context.hot_reload_enabled));
        reload.addMember("pending", choc::value::createBool(context.hot_reload_pending));
        obj.addMember("hotReload", reload);
        obj.addMember("unsavedTweakCount", choc::value::createInt64(static_cast<std::int64_t>(
                                               context.unsaved_tweak_count)));
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
        if (!tweak_store_)
            return make_error(req.id, "No tweak store attached");
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
        if (!tweak_store_)
            return make_error(req.id, "No tweak store attached");
        auto records = tweak_store_->list_tweaks();

        // Build the response as { tweaks: { anchor: { path: value } },
        // bypassed: { anchor: true | [paths] } } — mirrors the on-disk
        // TweaksFile schema.
        auto tweaks_obj = choc::value::createObject("");
        std::unordered_map<std::string, choc::value::Value> anchor_objs;
        for (auto& rec : records) {
            auto it = anchor_objs.find(rec.anchor_id);
            if (it == anchor_objs.end()) {
                it = anchor_objs.emplace(rec.anchor_id, choc::value::createObject("")).first;
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
        for (auto& rec : records)
            all_anchors.insert(rec.anchor_id);
        for (auto& anchor : tweak_store_->bypassed_anchors())
            all_anchors.insert(std::move(anchor));
        for (auto& anchor : all_anchors) {
            auto b = tweak_store_->bypass_for(anchor);
            if (!b)
                continue;
            std::visit(
                [&](auto&& v) {
                    using T = std::decay_t<decltype(v)>;
                    if constexpr (std::is_same_v<T, bool>) {
                        bypassed_obj.addMember(anchor, choc::value::createBool(v));
                    } else {
                        auto arr = choc::value::createEmptyArray();
                        for (auto& p : v)
                            arr.addArrayElement(choc::value::createString(p));
                        bypassed_obj.addMember(anchor, arr);
                    }
                },
                *b);
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
        if (!tweak_store_)
            return make_error(req.id, "No tweak store attached");
        try {
            auto params = req.params_json.empty() || req.params_json == "{}"
                              ? choc::value::createObject("")
                              : choc::json::parse(req.params_json);
            std::size_t removed = 0;
            bool has_anchor = params.isObject() && params.hasObjectMember("anchorId") &&
                              params["anchorId"].isString();
            bool has_path = params.isObject() && params.hasObjectMember("propertyPath") &&
                            params["propertyPath"].isString();
            if (has_anchor && has_path) {
                removed =
                    tweak_store_->remove_tweak(std::string(params["anchorId"].getString()),
                                               std::string(params["propertyPath"].getString()))
                        ? 1
                        : 0;
            } else if (has_anchor) {
                removed = tweak_store_->remove_anchor(std::string(params["anchorId"].getString()));
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
        if (!tweak_store_)
            return make_error(req.id, "No tweak store attached");
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
            return make_error(req.id, std::string("Inspector.loadTweaks failed: ") + r.error);
        }
        auto resp = choc::value::createObject("");
        resp.addMember("ok", choc::value::createBool(true));
        resp.addMember("path", choc::value::createString(r.path));
        resp.addMember("tweakCount", choc::value::createInt64(static_cast<int64_t>(r.tweak_count)));
        resp.addMember("bypassCount",
                       choc::value::createInt64(static_cast<int64_t>(r.bypass_count)));
        return make_response(req.id, choc::json::toString(resp, false));
    }
    if (req.method == methods::kInspectorSaveTweaks) {
        if (!tweak_store_)
            return make_error(req.id, "No tweak store attached");
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
            return make_error(req.id, std::string("Inspector.saveTweaks failed: ") + r.error);
        }
        auto resp = choc::value::createObject("");
        resp.addMember("ok", choc::value::createBool(true));
        resp.addMember("path", choc::value::createString(r.path));
        resp.addMember("tweakCount", choc::value::createInt64(static_cast<int64_t>(r.tweak_count)));
        resp.addMember("bypassCount",
                       choc::value::createInt64(static_cast<int64_t>(r.bypass_count)));
        return make_response(req.id, choc::json::toString(resp, false));
    }
    if (req.method == methods::kInspectorSetAutoSave) {
        if (!tweak_store_)
            return make_error(req.id, "No tweak store attached");
        bool enabled = false;
        std::string path;
        try {
            auto params = choc::json::parse(req.params_json);
            if (!params.isObject() || !params.hasObjectMember("enabled") ||
                !params["enabled"].isBool()) {
                return make_error(req.id, "Inspector.setAutoSave requires `enabled` as bool");
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
            if (!params.isObject() || !params.hasObjectMember("template") ||
                !params["template"].isString()) {
                return make_error(req.id,
                                  "Inspector.setEditorUrlTemplate requires `template` as string");
            }
            std::string tmpl = std::string(params["template"].getString());
            std::string err;
            if (!validate_editor_url_template(tmpl, &err))
                return make_error(req.id, std::string("Inspector.setEditorUrlTemplate: ") + err);
            config_.editor_url_template = tmpl;
            // Keep the overlay's `J`-hotkey config in sync so a runtime
            // template change reaches both jump paths.
            if (overlay_)
                overlay_->set_config(config_);
            auto resp = choc::value::createObject("");
            resp.addMember("ok", choc::value::createBool(true));
            resp.addMember("template", choc::value::createString(tmpl));
            return make_response(req.id, choc::json::toString(resp, false));
        } catch (const std::exception& e) {
            return make_error(req.id,
                              std::string("Invalid params for Inspector.setEditorUrlTemplate: ") +
                                  e.what());
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
        resp.addMember("configTemplate", choc::value::createString(config_.editor_url_template));
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
                    if (params.hasObjectMember("dryRun") && params["dryRun"].isBool())
                        dry_run = params["dryRun"].getBool();
                    if (params.hasObjectMember("anchorId") && params["anchorId"].isString())
                        anchor_id = std::string(params["anchorId"].getString());
                }
            } catch (...) {
                return make_error(req.id, "Invalid params for Inspector.jumpToSource");
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
            resp.addMember("line", choc::value::createInt64(static_cast<int64_t>(result.line)));
            resp.addMember("col", choc::value::createInt64(static_cast<int64_t>(result.col)));
            resp.addMember("launched", choc::value::createBool(result.launched));
            resp.addMember("dryRun", choc::value::createBool(dry_run));
            if (!result.error.empty())
                resp.addMember("launchError", choc::value::createString(result.error));
        } else {
            resp.addMember("error",
                           choc::value::createString(
                               anchor_id.empty() && !overlay_
                                   ? std::string("no overlay attached and no anchorId given")
                                   : result.error));
        }
        return make_response(req.id, choc::json::toString(resp, false));
    }
    if (req.method == methods::kInspectorSetBypass) {
        if (!tweak_store_)
            return make_error(req.id, "No tweak store attached");
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
                    if (arr[i].isString())
                        paths.push_back(std::string(arr[i].getString()));
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
        if (!tweak_store_)
            return make_error(req.id, "No tweak store attached");
        try {
            auto params = choc::json::parse(req.params_json);
            auto anchor = std::string(params["anchorId"].getString());
            if (!params.hasObjectMember("value") || !params["value"].isBool()) {
                return make_error(req.id, "Inspector.setLocked requires `value` as bool");
            }
            tweak_store_->set_locked(anchor, params["value"].getBool());
            auto resp = choc::value::createObject("");
            resp.addMember("ok", choc::value::createBool(true));
            resp.addMember("locked", choc::value::createBool(tweak_store_->is_locked(anchor)));
            return make_response(req.id, choc::json::toString(resp, false));
        } catch (...) {
            return make_error(req.id, "Invalid params for Inspector.setLocked");
        }
    }

    return make_error(req.id, "Unknown Inspector method: " + req.method);
}

} // namespace pulp::inspect
