// script_inspector_bridge.cpp — see script_inspector_bridge.hpp for the design.

#include <pulp/view/script_inspector_bridge.hpp>

#include <pulp/view/js_engine.hpp>
#include <pulp/view/script_engine.hpp>

#include <choc/containers/choc_Value.h>
#include <choc/text/choc_JSON.h>

#include <algorithm>

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
        (void)choc::json::parse("[" + json + "]");
    } catch (...) {
        return "null";
    }
    return json;
}
}  // namespace

ScriptInspectorBridge::~ScriptInspectorBridge() {
    detach();
}

void ScriptInspectorBridge::attach(ScriptEngine* engine) {
    // Replacing an attachment first fences any old queued/running request. In
    // the normal lifecycle this is a cheap no-op; on reload it prevents a late
    // completion from touching the replacement engine.
    detach();
    std::lock_guard<std::mutex> lock(mutex_);
    engine_ = engine;
    engine_thread_ = std::this_thread::get_id();
    have_engine_thread_ = true;
    caps_ = Capabilities{};
    if (engine) {
        caps_.engine = std::string(engine_type_name(engine->engine_type()));
        caps_.can_interrupt = engine->supports_interrupt();
        // This bridge promises a deadline on every path. A backend without a
        // cross-thread interrupt seam cannot safely offer Runtime.evaluate.
        caps_.can_evaluate = caps_.can_interrupt;
        // can_break / can_step / can_inspect_locals stay false — mainline
        // QuickJS exposes no source-line breakpoint or scope-inspection API.
    }
}

void ScriptInspectorBridge::detach() {
    std::shared_ptr<Request> stranded;
    std::shared_ptr<Request> running;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        engine_ = nullptr;
        caps_ = Capabilities{};
        stranded = std::move(pending_);
        if (stranded) {
            EvalResult result;
            result.detached = true;
            result.error = "engine detached before evaluation ran";
            finish_locked(stranded, std::move(result));
        }
        if (running_) {
            running_->detach_requested = true;
            running = running_;
        }
        if (running)
            interrupt_if_active_locked(running);

        state_cv_.wait(lock, [&] { return !running_; });
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
    ScriptEngine* engine = nullptr;
    bool owner_thread = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!engine_) {
            EvalResult r;
            r.detached = true;
            r.error = "no engine attached";
            return r;
        }
        if (in_flight_) {
            EvalResult r;
            r.busy = true;
            r.error = "an evaluation is already in flight";
            return r;
        }
        if (!caps_.can_interrupt) {
            EvalResult r;
            r.error = "attached engine cannot enforce an evaluation deadline";
            return r;
        }
        in_flight_ = true;
        req = std::make_shared<Request>();
        req->code = code;
        engine = engine_;
        owner_thread = have_engine_thread_ && std::this_thread::get_id() == engine_thread_;
        if (owner_thread) {
            req->state = RequestState::running;
            req->engine = engine;
            req->can_interrupt = caps_.can_interrupt;
            req->interrupt_window_open.store(true, std::memory_order_release);
            running_ = req;
        } else {
            pending_ = req;
        }
        state_cv_.notify_all();
    }

    const auto deadline = std::chrono::steady_clock::now()
                        + std::max(timeout, std::chrono::milliseconds::zero());

    if (owner_thread) {
        std::thread watchdog([this, req, deadline] {
            std::unique_lock<std::mutex> lock(mutex_);
            if (req->cv.wait_until(lock, deadline,
                                   [&] { return req->state == RequestState::finished; }))
                return;
            req->timeout_requested = true;
            interrupt_if_active_locked(req);
        });

        EvalResult result = serialize_eval(engine, code);
        req->interrupt_window_open.exchange(false, std::memory_order_acq_rel);
        {
            std::lock_guard<std::mutex> lock(mutex_);
            if (req->detach_requested) {
                result = EvalResult{};
                result.detached = true;
                result.error = "engine detached during evaluation";
            } else if (req->timeout_requested) {
                result = EvalResult{};
                result.timed_out = true;
                result.error = "evaluation timed out";
            } else if (req->interrupt_requested) {
                result = EvalResult{};
                result.error = "evaluation interrupted";
            }
            finish_locked(req, std::move(result));
        }
        watchdog.join();
        return req->result;
    }

    std::unique_lock<std::mutex> lock(mutex_);
    if (req->cv.wait_until(lock, deadline,
                           [&] { return req->state == RequestState::finished; }))
        return req->result;

    EvalResult timeout_result;
    timeout_result.timed_out = true;
    timeout_result.error = "evaluation timed out";
    if (req->state == RequestState::queued) {
        pending_.reset();
        finish_locked(req, timeout_result);
        return req->result;
    }

    req->timeout_requested = true;
    interrupt_if_active_locked(req);
    return timeout_result;
}

ScriptInspectorBridge::Capabilities ScriptInspectorBridge::capabilities() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return caps_;
}

bool ScriptInspectorBridge::interrupt() {
    std::unique_lock<std::mutex> lock(mutex_);
    if (!in_flight_)
        return false;
    if (pending_) {
        auto request = std::move(pending_);
        EvalResult result;
        result.error = "evaluation interrupted before it ran";
        finish_locked(request, std::move(result));
        return true;
    }
    if (!running_ || !running_->can_interrupt)
        return false;
    auto request = running_;
    if (!request->interrupt_window_open.exchange(false, std::memory_order_acq_rel))
        return false;
    request->interrupt_requested = true;
    auto* engine = request->engine;
    engine->request_interrupt();
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
        req->state = RequestState::running;
        req->engine = engine_;
        req->can_interrupt = caps_.can_interrupt;
        req->interrupt_window_open.store(true, std::memory_order_release);
        running_ = req;
        engine = engine_;
        state_cv_.notify_all();
    }

    EvalResult result;
    if (engine) {
        result = serialize_eval(engine, req->code);
    } else {
        result.detached = true;
        result.error = "engine detached before evaluation ran";
    }
    req->interrupt_window_open.exchange(false, std::memory_order_acq_rel);

    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (req->detach_requested) {
            result = EvalResult{};
            result.detached = true;
            result.error = "engine detached during evaluation";
        } else if (req->timeout_requested) {
            result = EvalResult{};
            result.timed_out = true;
            result.error = "evaluation timed out";
        } else if (req->interrupt_requested) {
            result = EvalResult{};
            result.error = "evaluation interrupted";
        }
        finish_locked(req, std::move(result));
    }
    return true;
}

bool ScriptInspectorBridge::is_busy() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return in_flight_;
}

void ScriptInspectorBridge::finish_locked(const std::shared_ptr<Request>& request,
                                           EvalResult result) {
    request->result = std::move(result);
    request->state = RequestState::finished;
    if (pending_ == request) pending_.reset();
    if (running_ == request) running_.reset();
    in_flight_ = false;
    request->cv.notify_all();
    state_cv_.notify_all();
}

bool ScriptInspectorBridge::interrupt_if_active_locked(
    const std::shared_ptr<Request>& request) {
    if (!request->can_interrupt || !request->engine)
        return false;
    if (!request->interrupt_window_open.exchange(false, std::memory_order_acq_rel))
        return false;
    request->engine->request_interrupt();
    return true;
}

} // namespace pulp::view
