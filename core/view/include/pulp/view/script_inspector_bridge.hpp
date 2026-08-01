// script_inspector_bridge.hpp — thread-marshaling seam between an off-thread
// inspector and the single-threaded scripted-UI JS engine.
#pragma once

#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>

namespace pulp::view {

class ScriptEngine;

// Lets an inspector running on a *background* thread evaluate expressions
// against a ScriptEngine that lives on the UI / engine-owner thread, query the
// engine's honest debug capabilities, and cooperatively abort a runaway
// evaluation — without ever touching the (not-thread-safe) engine off-thread.
//
// The engine thread is whichever thread calls attach()/pump()/detach(). It
// drains queued evaluate requests in pump(), which a host runs once per frame
// from ScriptedUiSession::poll(). A background evaluate() enqueues a request
// and blocks (with a timeout) until pump() fulfills it; if evaluate() is itself
// called on the engine thread it runs inline, with a watchdog using the
// backend's cross-thread interrupt seam to enforce the same deadline. Only one
// evaluation is in flight at a time — including owner-thread calls — and a
// concurrent request returns `busy`.
//
// This is deliberately NOT a step debugger: mainline QuickJS (Pulp's bundled
// engine) exposes no source-line breakpoint / stepping / local-scope API, so
// capabilities() reports can_break/can_step/can_inspect_locals = false. It is
// an honest runtime inspector: evaluate, capability reporting, and interrupt.
class ScriptInspectorBridge {
public:
    struct EvalResult {
        bool ok = false;          ///< evaluation completed without throwing
        bool timed_out = false;   ///< the request exceeded its deadline
        bool busy = false;        ///< another evaluation was already in flight
        bool detached = false;    ///< no engine is attached
        std::string json;         ///< result serialized as JSON when ok
        std::string error;        ///< message when !ok (exception text / reason)
    };

    // Honest snapshot of what the attached engine can do for debugging. Fixed
    // at attach() time from the engine's immutable identity, so it can be read
    // from any thread without touching the engine.
    struct Capabilities {
        std::string engine;              ///< "QuickJS" / "JavaScriptCore" / "V8" / ""
        bool can_evaluate = false;       ///< bounded Runtime.evaluate is available
        bool can_interrupt = false;      ///< a runaway eval can be aborted
        bool can_break = false;          ///< source-line breakpoints (never on mainline QuickJS)
        bool can_step = false;           ///< step in/over/out
        bool can_inspect_locals = false; ///< paused local-scope inspection
    };

    static constexpr std::chrono::milliseconds kDefaultTimeout{2000};

    ScriptInspectorBridge() = default;
    ~ScriptInspectorBridge();

    ScriptInspectorBridge(const ScriptInspectorBridge&) = delete;
    ScriptInspectorBridge& operator=(const ScriptInspectorBridge&) = delete;

    // [engine thread] Attach the engine and capture its capability snapshot.
    // Passing a new engine replaces the previous one; the calling thread is
    // recorded as the engine thread for inline-eval detection.
    void attach(ScriptEngine* engine);

    // [engine thread] Detach the engine. Queued work fails with `detached`;
    // running work is interrupted and detach waits for engine quiescence.
    void detach();

    // [engine thread] Install the host-owned realm reset run after evaluated
    // code has stopped but before its result is published. Returning a
    // non-empty string fails the request closed with that reset error.
    using EvaluationDeadline = std::chrono::steady_clock::time_point;
    void set_post_evaluation_reset(
        std::function<std::string(EvaluationDeadline)> reset);

    // [any thread] Evaluate `code`, marshaled to the engine thread. Blocks up
    // to `timeout`. On timeout the running evaluation is interrupted (if the
    // engine supports it) so a runaway loop cannot wedge the bridge forever.
    EvalResult evaluate(const std::string& code,
                        std::chrono::milliseconds timeout = kDefaultTimeout,
                        std::size_t max_result_bytes = 1024 * 1024);

    // [any thread] Capability snapshot (empty engine name when detached).
    Capabilities capabilities() const;

    // [any thread] Cancel queued work or cooperatively abort running work.
    // Returns false when there is no cancellable request.
    bool interrupt();

    // [engine thread] Drain the pending evaluate request, if any. Returns true
    // when a request was serviced. Host calls this once per frame.
    bool pump();

    // [any thread] Whether an evaluation is currently queued or executing.
    bool is_busy() const;

private:
    enum class RequestState { queued, running, finished };

    struct Request {
        std::string code;
        std::size_t max_result_bytes = 0;
        std::condition_variable cv;
        RequestState state = RequestState::queued;
        bool timeout_requested = false;
        bool detach_requested = false;
        bool interrupt_requested = false;
        bool evaluation_finished = false;
        EvaluationDeadline deadline{};
        ScriptEngine* engine = nullptr;
        bool can_interrupt = false;
        // Protected by mutex_. A cancelling thread closes the window and arms
        // the engine interrupt while holding that mutex; the engine thread
        // takes the same mutex before closing/draining at quiescence. That
        // ordering prevents a cancel flag from being armed after the drain.
        bool interrupt_window_open = false;
        EvalResult result;
    };

    // Runs engine->evaluate on the engine thread, serializing the result or the
    // thrown exception into an EvalResult. Takes the engine explicitly so the
    // caller's snapshot is the one used. Caller guarantees engine-thread.
    EvalResult serialize_eval(ScriptEngine* engine, const std::string& code,
                              std::size_t max_result_bytes) const;
    // [engine thread] Execute, reset, normalize, and publish one already
    // claimed request. Inline evaluation and pump share this state machine.
    EvalResult run_claimed_request(const std::shared_ptr<Request>& request,
                                   ScriptEngine* engine);

    // mutex_ must be held. Publishes a terminal result and releases the
    // single-flight slot. Request waiters use the same mutex, so completion and
    // cancellation have one ordering point.
    void finish_locked(const std::shared_ptr<Request>& request, EvalResult result);
    // mutex_ must be held. The engine interrupt seam is explicitly nonblocking;
    // retaining the lock through the call prevents detach() from returning and
    // the caller-owned engine from being destroyed before the dereference.
    static bool interrupt_if_active_locked(const std::shared_ptr<Request>& request);

    friend struct ScriptInspectorBridgeTestAccess;

    mutable std::mutex mutex_;
    std::condition_variable state_cv_;
    ScriptEngine* engine_ = nullptr;
    Capabilities caps_{};
    std::thread::id engine_thread_{};
    bool have_engine_thread_ = false;
    bool in_flight_ = false;
    bool reset_in_progress_ = false;
    std::thread::id reset_thread_{};
    std::shared_ptr<Request> pending_;  // single-slot queue, drained by pump()
    std::shared_ptr<Request> running_;  // engine remains attached until this clears
    std::function<std::string(EvaluationDeadline)> post_evaluation_reset_;
};

} // namespace pulp::view
