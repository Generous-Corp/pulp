// script_inspector_bridge.cpp — see script_inspector_bridge.hpp for the design.

#include <pulp/view/script_inspector_bridge.hpp>

#include <pulp/view/js_engine.hpp>
#include <pulp/view/script_engine.hpp>

#include <algorithm>

namespace pulp::view {

namespace {
constexpr auto kPostEvaluationResetGrace = std::chrono::milliseconds(500);

std::string invoke_realm_reset(
    const std::function<std::string(ScriptInspectorBridge::EvaluationDeadline)>& reset,
    ScriptInspectorBridge::EvaluationDeadline deadline) noexcept {
    if (!reset)
        return {};
    try {
        return reset(deadline);
    } catch (const std::exception& e) {
        return e.what();
    } catch (...) {
        return "unknown realm reset error";
    }
}
}

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
        caps_.can_evaluate =
            caps_.can_interrupt && engine->supports_bounded_json_evaluation();
        // can_break / can_step / can_inspect_locals stay false — mainline
        // QuickJS exposes no source-line breakpoint or scope-inspection API.
    }
}

void ScriptInspectorBridge::detach() {
    std::shared_ptr<Request> stranded;
    std::shared_ptr<Request> running;
    {
        std::unique_lock<std::mutex> lock(mutex_);
        const bool reset_reentry =
            reset_in_progress_ && std::this_thread::get_id() == reset_thread_;
        engine_ = nullptr;
        caps_ = Capabilities{};
        // The detach/attach pair inside a realm reset is an engine SWAP, not a
        // teardown, so it must not fail a request that is merely queued: with
        // the reset deferred to the frame boundary, the whole gap between one
        // evaluation's response and the next frame is time in which a client
        // can queue, and stranding there would fail back-to-back evaluations
        // for no reason. The request has not run yet and the replacement engine
        // serves it in this same frame's pump. A reset that cannot produce a
        // replacement strands it explicitly instead — see
        // run_pending_post_evaluation_reset().
        if (!reset_reentry) {
            stranded = std::move(pending_);
            if (stranded) {
                EvalResult result;
                result.detached = true;
                result.error = "engine detached before evaluation ran";
                finish_locked(stranded, std::move(result));
            }
        }
        if (running_ && !reset_reentry) {
            running_->detach_requested = true;
            running = running_;
        }
        if (running)
            interrupt_if_active_locked(running);

        // A deferred reset runs outside any request, so quiescence means both
        // "no evaluation running" and "no realm reconstruction in flight" — a
        // detacher must not return while the reset is still touching the engine.
        if (!reset_reentry)
            state_cv_.wait(lock, [&] { return !running_ && !reset_in_progress_; });
    }
}

void ScriptInspectorBridge::set_post_evaluation_reset(
    std::function<std::string(EvaluationDeadline)> reset) {
    std::lock_guard<std::mutex> lock(mutex_);
    post_evaluation_reset_ = std::move(reset);
}

bool ScriptInspectorBridge::post_evaluation_reset_pending() const {
    std::lock_guard<std::mutex> lock(mutex_);
    return reset_pending_;
}

std::string ScriptInspectorBridge::run_pending_post_evaluation_reset() {
    std::function<std::string(EvaluationDeadline)> reset;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (!reset_pending_)
            return {};
        reset_pending_ = false;
        reset = post_evaluation_reset_;
        reset_in_progress_ = true;
        reset_thread_ = std::this_thread::get_id();
    }
    // Realm reconstruction stays mandatory after every evaluation and keeps its
    // own small, fixed grace window. Deferral moved the work off the request, so
    // the window runs from this call rather than from the request deadline that
    // has already expired by the time the host reaches its frame boundary.
    const auto reset_error = invoke_realm_reset(
        reset, std::chrono::steady_clock::now() + kPostEvaluationResetGrace);
    {
        std::lock_guard<std::mutex> lock(mutex_);
        reset_in_progress_ = false;
        reset_thread_ = {};
        // A reset the host held its queued slot through, but which produced no
        // replacement engine, has nothing left to serve that request with. Fail
        // it now rather than let it wait out its own deadline holding the
        // single-flight slot.
        if (!engine_ && pending_) {
            auto stranded = std::move(pending_);
            EvalResult result;
            result.detached = true;
            result.error = "engine detached before evaluation ran";
            finish_locked(stranded, std::move(result));
        }
        state_cv_.notify_all();
    }
    return reset_error;
}

ScriptInspectorBridge::EvalResult
ScriptInspectorBridge::serialize_eval(ScriptEngine* engine, const std::string& code,
                                      std::size_t max_result_bytes) const {
    EvalResult r;
    if (!engine) {
        r.detached = true;
        r.error = "no engine attached";
        return r;
    }
    try {
        r.ok = true;
        r.json = engine->evaluate_bounded_json(code, max_result_bytes);
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
ScriptInspectorBridge::evaluate(const std::string& code,
                                std::chrono::milliseconds timeout,
                                std::size_t max_result_bytes) {
    const auto deadline = std::chrono::steady_clock::now()
                        + std::max(timeout, std::chrono::milliseconds::zero());
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
        req->max_result_bytes = max_result_bytes;
        req->deadline = deadline;
        engine = engine_;
        owner_thread = have_engine_thread_ && std::this_thread::get_id() == engine_thread_;
        if (owner_thread) {
            req->state = RequestState::running;
            req->engine = engine;
            req->can_interrupt = caps_.can_interrupt;
            req->interrupt_window_open = true;
            running_ = req;
        } else {
            pending_ = req;
        }
        state_cv_.notify_all();
    }

    if (owner_thread)
        return run_claimed_request(req, engine);

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
    // A re-entrant call from an engine native callback cannot wait for the
    // surrounding evaluation to acknowledge cancellation without deadlocking
    // that same thread. The inspector server invokes this from its reader
    // thread; fail honestly for unsupported owner-thread re-entry.
    if (have_engine_thread_ && std::this_thread::get_id() == engine_thread_)
        return false;
    auto request = running_;
    if (!request->interrupt_window_open)
        return false;
    request->interrupt_window_open = false;
    request->interrupt_requested = true;
    auto* engine = request->engine;
    engine->request_interrupt();
    // A request is only reported as interrupted if the backend consumes it.
    // Wait for the engine thread to distinguish that from an interrupt which
    // arrived after evaluation's final check and was merely cleared.
    request->cv.wait(lock, [&] { return request->evaluation_finished; });
    return request->interrupt_consumed;
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
        req->interrupt_window_open = true;
        running_ = req;
        engine = engine_;
        state_cv_.notify_all();
    }

    (void)run_claimed_request(req, engine);
    return true;
}

ScriptInspectorBridge::EvalResult ScriptInspectorBridge::run_claimed_request(
    const std::shared_ptr<Request>& req, ScriptEngine* engine) {
    std::thread watchdog([this, req] {
        std::unique_lock<std::mutex> lock(mutex_);
        if (req->cv.wait_until(lock, req->deadline,
                               [&] { return req->evaluation_finished; }))
            return;
        req->timeout_requested = true;
        interrupt_if_active_locked(req);
    });

    EvalResult result;
    if (engine)
        result = serialize_eval(engine, req->code, req->max_result_bytes);
    else {
        result.detached = true;
        result.error = "engine detached before evaluation ran";
    }
    bool interrupt_was_issued = false;
    {
        std::lock_guard<std::mutex> lock(mutex_);
        interrupt_was_issued = !req->interrupt_window_open;
        req->interrupt_window_open = false;
    }
    bool interrupt_was_late = false;
    if (interrupt_was_issued && engine)
        interrupt_was_late = engine->clear_pending_interrupt();

    {
        std::lock_guard<std::mutex> lock(mutex_);
        req->interrupt_consumed =
            req->interrupt_requested && !interrupt_was_late;
        req->evaluation_finished = true;
        // Evaluated code may have planted timers, animation frames, Promise
        // jobs, or patched callbacks, so the realm that ran it must be rebuilt
        // before anything can execute them. Rebuilding HERE would replace the
        // view tree underneath a caller that is still holding widget pointers
        // from before the request. Record the debt instead and let the host
        // discharge it at its frame boundary: nothing evaluated gets a frame
        // either way, which is the containment the reset exists to provide.
        if (post_evaluation_reset_)
            reset_pending_ = true;
        req->cv.notify_all();
    }
    watchdog.join();
    {
        std::lock_guard<std::mutex> lock(mutex_);
        if (req->detach_requested) {
            engine_ = nullptr;
            caps_ = Capabilities{};
            result = EvalResult{};
            result.detached = true;
            result.error = "engine detached during evaluation";
        } else if (req->timeout_requested) {
            result = EvalResult{};
            result.timed_out = true;
            result.error = "evaluation timed out";
        } else if (req->interrupt_consumed) {
            result = EvalResult{};
            result.error = "evaluation interrupted";
        }
        finish_locked(req, std::move(result));
    }
    return req->result;
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
    if (!request->interrupt_window_open)
        return false;
    request->interrupt_window_open = false;
    request->engine->request_interrupt();
    return true;
}

} // namespace pulp::view
