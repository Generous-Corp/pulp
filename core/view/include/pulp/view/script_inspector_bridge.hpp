// script_inspector_bridge.hpp — thread-marshaling seam between an off-thread
// inspector and the single-threaded scripted-UI JS engine.
#pragma once

#include <chrono>
#include <condition_variable>
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
// called on the engine thread it runs inline to avoid deadlocking on a pump
// that can never come. Only one evaluation is in flight at a time — a
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
        bool timed_out = false;   ///< the engine thread did not drain in time
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
        bool can_evaluate = false;       ///< Runtime.evaluate is wired
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

    // [engine thread] Detach the engine. Any evaluation that has not yet been
    // picked up by pump() will fail with `detached`.
    void detach();

    // [any thread] Evaluate `code`, marshaled to the engine thread. Blocks up
    // to `timeout`. On timeout the running evaluation is interrupted (if the
    // engine supports it) so a runaway loop cannot wedge the bridge forever.
    EvalResult evaluate(const std::string& code,
                        std::chrono::milliseconds timeout = kDefaultTimeout);

    // [any thread] Capability snapshot (empty engine name when detached).
    Capabilities capabilities() const;

    // [any thread] Cooperatively abort the in-flight evaluation. Returns false
    // if nothing is running, the engine can't interrupt, or none is attached.
    bool interrupt();

    // [engine thread] Drain the pending evaluate request, if any. Returns true
    // when a request was serviced. Host calls this once per frame.
    bool pump();

    // [any thread] Whether an evaluation is currently queued or executing.
    bool is_busy() const;

private:
    struct Request {
        std::string code;
        std::mutex m;
        std::condition_variable cv;
        bool done = false;
        EvalResult result;
    };

    // Runs engine->evaluate on the engine thread, serializing the result or the
    // thrown exception into an EvalResult. Takes the engine explicitly so the
    // caller's snapshot is the one used. Caller guarantees engine-thread.
    EvalResult serialize_eval(ScriptEngine* engine, const std::string& code) const;

    mutable std::mutex mutex_;
    ScriptEngine* engine_ = nullptr;
    Capabilities caps_{};
    std::thread::id engine_thread_{};
    bool have_engine_thread_ = false;
    bool in_flight_ = false;
    std::shared_ptr<Request> pending_;  // single-slot queue, drained by pump()
};

} // namespace pulp::view
