// script_inspector_bridge.cpp — see script_inspector_bridge.hpp for the design.

#include <pulp/view/script_inspector_bridge.hpp>

#include <pulp/view/js_engine.hpp>
#include <pulp/view/script_engine.hpp>

#include <choc/containers/choc_Value.h>
#include <choc/text/choc_JSON.h>

namespace pulp::view {

namespace {
// Serialize a value to JSON that is guaranteed valid so it can be embedded raw
// in the response frame. choc renders non-finite numbers (NaN / Infinity from
// e.g. `1/0`) as bare tokens that are not valid JSON; if the render doesn't
// round-trip through the parser, fall back to null rather than emit a response
// the client can't parse.
std::string value_to_json(const choc::value::Value& value) {
    if (value.isVoid()) return "null";
    std::string json = choc::json::toString(value);
    // Validate that the render is parseable JSON before embedding it raw. Wrap
    // in an array because choc::json::parse rejects bare top-level scalars
    // (`42`, `"abc"`) — the wrap accepts those yet still rejects non-finite
    // number tokens (NaN / Infinity from e.g. `1/0`), which we replace with null.
    try {
        choc::json::parse("[" + json + "]");
    } catch (...) {
        return "null";
    }
    return json;
}
}  // namespace

ScriptInspectorBridge::~ScriptInspectorBridge() {
    detach();  // wake any waiter blocked on a pending request
}

void ScriptInspectorBridge::attach(ScriptEngine* engine) {
    std::lock_guard<std::mutex> lock(mutex_);
    engine_ = engine;
    engine_thread_ = std::this_thread::get_id();
    have_engine_thread_ = true;
    caps_ = Capabilities{};
    if (engine) {
        caps_.engine = std::string(engine_type_name(engine->engine_type()));
        caps_.can_evaluate = true;
        caps_.can_interrupt = engine->supports_interrupt();
        // can_break / can_step / can_inspect_locals stay false — mainline
        // QuickJS exposes no source-line breakpoint or scope-inspection API.
    }
}

void ScriptInspectorBridge::detach() {
    std::shared_ptr<Request> stranded;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        engine_ = nullptr;
        caps_ = Capabilities{};
        stranded = std::move(pending_);
        pending_.reset();
        in_flight_ = false;
    }
    if (stranded) {
        {
            std::lock_guard<std::mutex> l(stranded->m);
            stranded->result = EvalResult{};
            stranded->result.detached = true;
            stranded->result.error = "engine detached before evaluation ran";
            stranded->done = true;
        }
        stranded->cv.notify_all();
    }
}

ScriptInspectorBridge::EvalResult
ScriptInspectorBridge::serialize_eval(ScriptEngine* engine, const std::string& code) const {
    EvalResult r;
    if (!engine) {
        r.detached = true;
        r.error = "no engine attached";
        return r;
    }
    try {
        choc::value::Value value = engine->evaluate(code);
        r.ok = true;
        r.json = value_to_json(value);
    } catch (const std::exception& e) {
        r.ok = false;
        r.error = e.what();
    } catch (...) {
        r.ok = false;
        r.error = "unknown evaluation error";
    }
    return r;
}

ScriptInspectorBridge::EvalResult
ScriptInspectorBridge::evaluate(const std::string& code, std::chrono::milliseconds timeout) {
    std::shared_ptr<Request> req;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!engine_) {
            EvalResult r;
            r.detached = true;
            r.error = "no engine attached";
            return r;
        }
        // Same-thread client: run inline. A pump() to fulfill a queued request
        // can never arrive on the thread that would have driven it, so queuing
        // here would deadlock. This inline path intentionally holds mutex_ across
        // the evaluation and is therefore not interruptible — a same-thread REPL
        // eval has no other thread to interrupt it anyway; the interruptible path
        // is the cross-thread pump() below.
        if (have_engine_thread_ && std::this_thread::get_id() == engine_thread_) {
            return serialize_eval(engine_, code);
        }
        if (in_flight_) {
            EvalResult r;
            r.busy = true;
            r.error = "an evaluation is already in flight";
            return r;
        }
        in_flight_ = true;
        req = std::make_shared<Request>();
        req->code = code;
        pending_ = req;
    }

    std::unique_lock<std::mutex> l(req->m);
    if (req->cv.wait_for(l, timeout, [&] { return req->done; }))
        return req->result;

    // Timed out waiting for the engine thread. Abort a runaway evaluation (if
    // the engine can) and give the abort a brief window to unwind before we
    // report the timeout. A late pump() completion after this point writes into
    // req (still alive via the caller's shared_ptr) but no one reads it.
    l.unlock();
    interrupt();
    l.lock();
    if (req->cv.wait_for(l, std::chrono::milliseconds(300), [&] { return req->done; }))
        return req->result;

    EvalResult r;
    r.timed_out = true;
    r.error = "evaluation timed out";
    return r;
}

ScriptInspectorBridge::Capabilities ScriptInspectorBridge::capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return caps_;
}

bool ScriptInspectorBridge::interrupt() {
    std::lock_guard<std::mutex> lock(mutex_);
    // Only arm the interrupt while an evaluation is actually in flight — arming
    // it idle would abort the *next* evaluation instead (QuickJS clears the
    // cancel flag on the next interrupt check, which only fires during JS).
    if (!engine_ || !caps_.can_interrupt || !in_flight_)
        return false;
    engine_->request_interrupt();  // atomic store; safe from this foreign thread
    return true;
}

bool ScriptInspectorBridge::pump() {
    std::shared_ptr<Request> req;
    ScriptEngine* engine = nullptr;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!have_engine_thread_) {
            engine_thread_ = std::this_thread::get_id();
            have_engine_thread_ = true;
        }
        if (!pending_)
            return false;
        req = std::move(pending_);
        pending_.reset();
        engine = engine_;  // stable: engine_ is only mutated on this (engine) thread
    }

    EvalResult result;
    if (engine) {
        result = serialize_eval(engine, req->code);
    } else {
        result.detached = true;
        result.error = "engine detached before evaluation ran";
    }

    {
        std::lock_guard<std::mutex> lock(mutex_);
        in_flight_ = false;
    }
    {
        std::lock_guard<std::mutex> l(req->m);
        req->result = std::move(result);
        req->done = true;
    }
    req->cv.notify_all();
    return true;
}

bool ScriptInspectorBridge::is_busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_;
}

} // namespace pulp::view
