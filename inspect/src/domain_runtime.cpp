// domain_runtime.cpp — Console and Runtime protocol domains for the
// scripted-UI runtime inspector (evaluate / capabilities / interrupt) and the
// device-log cursor poll. Split out of domain_handler.cpp so the dispatch hub
// stays under its size ceiling; these are DomainHandler member definitions and
// link into the same library.

#include <pulp/inspect/domain_handler.hpp>
#include <pulp/inspect/console_capture.hpp>
#include <pulp/view/script_inspector_bridge.hpp>

#include <choc/containers/choc_Value.h>
#include <choc/text/choc_JSON.h>

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::inspect {

// ── Console domain ──────────────────────────────────────────────────────────

namespace {
// Serialize console entries into a JSON array, carrying each entry's monotonic
// seq so a device-log client can page forward with Console.getMessages.
choc::value::Value console_entries_json(const std::vector<ConsoleCapture::Entry>& entries) {
    auto arr = choc::value::createEmptyArray();
    for (auto& e : entries) {
        auto obj = choc::value::createObject("");
        obj.addMember("level", choc::value::createString(e.level));
        obj.addMember("message", choc::value::createString(e.message));
        obj.addMember("seq", choc::value::createInt64(static_cast<int64_t>(e.seq)));
        arr.addArrayElement(obj);
    }
    return arr;
}
}  // namespace

InspectorMessage DomainHandler::handle_console(const InspectorMessage& req) {
    if (req.method == methods::kConsoleEnable) {
        if (console_)
            return make_response(req.id, choc::json::toString(console_entries_json(console_->entries()), false));
        return make_response(req.id, "[]");
    }
    // Console.getMessages { sinceSeq? } → { messages: [...], nextSeq } — the
    // device-log poll. Return only entries newer than the client's cursor plus
    // the next cursor to pass back, so a CLI/editor can tail logs without dupes.
    if (req.method == methods::kConsoleGetMessages) {
        if (!console_)
            return make_response(req.id, R"({"messages":[],"nextSeq":0})");
        uint64_t since = 0;
        if (!req.params_json.empty() && req.params_json != "{}") {
            try {
                auto params = choc::json::parse(req.params_json);
                if (params.isObject() && params.hasObjectMember("sinceSeq"))
                    since = static_cast<uint64_t>(params["sinceSeq"].getWithDefault(int64_t{0}));
            } catch (...) {
                return make_error(req.id, "Invalid params for Console.getMessages");
            }
        }
        uint64_t next_seq = 0;
        auto entries = console_->entries_since(since, next_seq);
        auto obj = choc::value::createObject("");
        obj.addMember("messages", console_entries_json(entries));
        obj.addMember("nextSeq", choc::value::createInt64(static_cast<int64_t>(next_seq)));
        return make_response(req.id, choc::json::toString(obj, false));
    }
    return make_error(req.id, "Unknown Console method: " + req.method);
}

// ── Runtime domain ──────────────────────────────────────────────────────────

InspectorMessage DomainHandler::handle_runtime(const InspectorMessage& req) {
    if (req.method == methods::kRuntimeEvaluate) {
        if (!runtime_eval_enabled_)
            return make_error(req.id, "Runtime.evaluate disabled (host has not opted into runtime eval)");
        if (!script_inspector_)
            return make_error(req.id, "Runtime.evaluate unavailable: no scripted-UI engine attached");
        std::string code;
        try {
            auto params = choc::json::parse(req.params_json);
            if (params.isObject()) {
                if (params.hasObjectMember("code"))
                    code = params["code"].getWithDefault(std::string{});
                else if (params.hasObjectMember("expression"))  // CDP-compatible alias
                    code = params["expression"].getWithDefault(std::string{});
            }
        } catch (...) {
            return make_error(req.id, "Invalid params for Runtime.evaluate");
        }
        if (code.empty())
            return make_error(req.id, "Runtime.evaluate requires a non-empty 'code'");

        auto result = script_inspector_->evaluate(code);
        if (result.ok) {
            // `result` is already a JSON value; embed it raw so numbers/objects
            // stay typed rather than getting re-stringified.
            return make_response(req.id, "{\"result\":" + result.json + "}");
        }
        if (result.detached)
            return make_error(req.id, "Runtime.evaluate unavailable: engine detached");
        if (result.busy)
            return make_error(req.id, "Runtime.evaluate busy: another evaluation is in flight");
        if (result.timed_out)
            return make_error(req.id, "Runtime.evaluate timed out (evaluation interrupted)");
        return make_error(req.id, result.error.empty() ? "Runtime.evaluate failed" : result.error);
    }
    if (req.method == methods::kRuntimeGetCapabilities) {
        view::ScriptInspectorBridge::Capabilities caps;
        if (script_inspector_) caps = script_inspector_->capabilities();
        auto obj = choc::value::createObject("");
        obj.addMember("engine", choc::value::createString(caps.engine));
        obj.addMember("attached", choc::value::createBool(script_inspector_ != nullptr && !caps.engine.empty()));
        // canEvaluate reflects BOTH the engine capability and the host opt-in —
        // a client must not attempt eval when the host hasn't enabled it.
        obj.addMember("canEvaluate", choc::value::createBool(caps.can_evaluate && runtime_eval_enabled_));
        obj.addMember("canInterrupt", choc::value::createBool(caps.can_interrupt && runtime_eval_enabled_));
        // Honest: mainline QuickJS has no source-line debug protocol, so these
        // stay false. A future debugger-enabled engine backend flips them.
        obj.addMember("canBreak", choc::value::createBool(caps.can_break));
        obj.addMember("canStep", choc::value::createBool(caps.can_step));
        obj.addMember("canInspectLocals", choc::value::createBool(caps.can_inspect_locals));
        return make_response(req.id, choc::json::toString(obj, false));
    }
    if (req.method == methods::kRuntimeInterrupt) {
        bool interrupted = runtime_eval_enabled_ && script_inspector_ && script_inspector_->interrupt();
        auto obj = choc::value::createObject("");
        obj.addMember("interrupted", choc::value::createBool(interrupted));
        return make_response(req.id, choc::json::toString(obj, false));
    }
    if (req.method == methods::kRuntimeGetHotReloadStatus) {
        return make_response(req.id, R"({"available":false})");
    }
    return make_error(req.id, "Unknown Runtime method: " + req.method);
}

} // namespace pulp::inspect
