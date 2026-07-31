// trace.cpp — process-global tracing controller (see trace_session.hpp).
//
// DEV ONLY. Under PULP_TRACING=OFF this compiles to trivial no-op stubs and
// pulls in zero Perfetto headers/symbols. Under ON it drives Perfetto's
// in-process backend: one process-wide session, call_once initialization
// (Initialize + Register are process singletons), reference-counted attachment
// for multi-instance hosts.

#include <pulp/runtime/trace_session.hpp>

#include <atomic>

#include <pulp/runtime/trace.hpp>  // PULP_TRACING_ENABLED + category storage decl

namespace pulp::runtime {

// Config-independent so the default OFF test build can exercise the one-shot
// semantics directly: the first caller wins the compare-exchange and gets true,
// every later caller sees the flag already set and gets false.
bool tracing_reminder_first_time(std::atomic<bool>& already_emitted) {
    bool expected = false;
    return already_emitted.compare_exchange_strong(expected, true,
                                                   std::memory_order_relaxed);
}

}  // namespace pulp::runtime

#if defined(PULP_TRACING_ENABLED) && PULP_TRACING_ENABLED

#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <chrono>
#include <cstring>
#include <memory>
#include <mutex>
#include <utility>

#include <pulp/runtime/crypto.hpp>
#include <pulp/runtime/log.hpp>
#include <pulp/runtime/trace_timeout.hpp>

// Track-event storage for the categories declared in trace.hpp. Exactly one TU.
PERFETTO_TRACK_EVENT_STATIC_STORAGE();

// Ship-guard sentinel. This byte-string is emitted ONLY into a binary compiled
// with PULP_TRACING=ON (this whole TU body is under `#if PULP_TRACING_ENABLED`).
// `pulp ship` scans candidate artifacts for these exact bytes and refuses to
// package a traced binary without `--allow-tracing`, so a dev build with
// Perfetto compiled in can never silently reach a customer. `used` + default
// visibility keep it past dead-strip; `volatile` reads block whole-program DCE.
// `used` + default visibility is GCC/Clang syntax; MSVC rejects it outright,
// which meant PULP_TRACING=ON had never compiled on Windows at all. dllexport
// is the MSVC equivalent for "this symbol must survive to the final binary":
// it keeps the bytes past dead-strip so `pulp ship`'s scanner can still find
// them, which is the entire point of the sentinel.
#if defined(_MSC_VER)
#  define PULP_TRACING_KEEP_SYMBOL __declspec(dllexport)
#else
#  define PULP_TRACING_KEEP_SYMBOL __attribute__((used, visibility("default")))
#endif
extern "C" PULP_TRACING_KEEP_SYMBOL
const char pulp_tracing_ship_sentinel[] = "PULP_TRACING_COMPILED_IN__DO_NOT_SHIP";

namespace pulp::runtime {

void log_tracing_reminder() {
    static std::atomic<bool> emitted{false};
    if (tracing_reminder_first_time(emitted)) {
        log_info(
            "Perfetto tracing compiled in (dev build) — not for release. "
            "Reconfigure with -DPULP_TRACING=OFF to disable.");
    }
}

namespace {

std::mutex g_mu;                                        // guards g_session/g_out
std::unique_ptr<perfetto::TracingSession> g_session;    // the one process session
std::string g_out_path;
std::uint64_t g_session_generation = 0;
std::uint64_t g_ownership_token = 0;
std::mutex g_attach_mu;
int g_refcount = 0;
std::shared_ptr<const TraceOwnership> g_attach_ownership;
std::once_flag g_init_once;

// The owned auto-flush timeout (see trace_timeout.hpp — it lives in a header
// so its lifetime rules are testable in the default PULP_TRACING=OFF build,
// which is the only build CI runs).
//
// Guarded by g_timeout_mu, NOT g_mu: the expiry callback takes g_mu to stop the
// session, so sharing one mutex would deadlock a cancel that races an expiry.
//
// Declared LAST in this block on purpose. Namespace-scope statics destroy in
// reverse declaration order, so ~TimeoutController — which joins a worker that
// may be inside stop() — runs while g_mu and g_session are still alive. That
// destructor is only a backstop for an unbalanced attach; the balanced path
// joins in the final detach() below.
std::mutex g_timeout_mu;
detail::TimeoutController g_timeout;

void ensure_initialized() {
    std::call_once(g_init_once, [] {
        perfetto::TracingInitArgs args;
        args.backends = perfetto::kInProcessBackend;
        perfetto::Tracing::Initialize(args);
        perfetto::TrackEvent::Register();
    });
}

std::string default_out_path() {
    if (const char* p = std::getenv("PULP_TRACE_PATH"); p && *p) return p;
    std::error_code ec;
    auto dir = std::filesystem::temp_directory_path(ec);
    if (ec) return "pulp-trace.pftrace";
    return (dir / "pulp-trace.pftrace").string();
}

std::optional<std::uint64_t> make_ownership_token() {
    for (;;) {
        const auto bytes = secure_random_bytes(sizeof(std::uint64_t));
        if (!bytes)
            return std::nullopt;
        std::uint64_t token = 0;
        std::memcpy(&token, bytes->data(), sizeof(token));
        if (token != 0)
            return token;
    }
}

}  // namespace

TraceStartResult Tracing::start_exclusive(
    const std::vector<std::string>& /*categories*/,
    const std::string& out_path, std::uint32_t ring_kb) {
    ensure_initialized();
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_session)
        return {TraceStartStatus::AlreadyActive, std::nullopt};

    perfetto::TraceConfig cfg;
    cfg.add_buffers()->set_size_kb(ring_kb);
    cfg.add_data_sources()->mutable_config()->set_name("track_event");
    // Selective category filtering lands with the inspector wire; for now all
    // declared categories are enabled (Perfetto's track_event default).

    // Duration cap so a crashed host still auto-flushes a bounded trace.
    if (const char* s = std::getenv("PULP_TRACE_SECONDS"); s && *s) {
        if (int secs = std::atoi(s); secs > 0)
            cfg.set_duration_ms(static_cast<std::uint32_t>(secs) * 1000u);
    }

    g_out_path = out_path.empty() ? default_out_path() : out_path;
    const auto ownership_token = make_ownership_token();
    if (!ownership_token)
        return {};
    auto session = perfetto::Tracing::NewTrace();
    session->Setup(cfg);
    session->StartBlocking();
    g_session = std::move(session);
    if (++g_session_generation == 0)
        ++g_session_generation;
    g_ownership_token = *ownership_token;
    return {
        TraceStartStatus::Started,
        TraceOwnership{g_session_generation, g_ownership_token},
    };
}

bool Tracing::start(const std::vector<std::string>& categories,
                    const std::string& out_path, std::uint32_t ring_kb) {
    return start_exclusive(categories, out_path, ring_kb).status !=
           TraceStartStatus::Unavailable;
}

namespace {
TraceStopResult stop_matching_owner(std::uint64_t expected_generation,
                                    std::uint64_t expected_token) {
    std::unique_ptr<perfetto::TracingSession> session;
    std::string path;
    // Hold the process-session lock through stop, readback, and persistence.
    // A replacement generation must not overlap Perfetto teardown or race the
    // preceding generation while writing the same configured output path.
    std::unique_lock<std::mutex> lk(g_mu);
    if (!g_session ||
        (expected_generation != 0 &&
         (expected_generation != g_session_generation ||
          expected_token != g_ownership_token))) {
        return {};
    }
    session = std::move(g_session);
    path = g_out_path;
    // Flush any buffered track events before reading (StopBlocking alone does
    // not guarantee the last packets are visible).
    perfetto::TrackEvent::Flush();
    session->StopBlocking();
    std::vector<char> data = session->ReadTraceBlocking();

    std::ofstream out(path, std::ios::binary);
    out.write(data.data(), static_cast<std::streamsize>(data.size()));

    TraceStopResult r;
    r.ok = out.good();
    r.path = path;
    r.trace_bytes = data.size();
    return r;
}
} // namespace

TraceStopResult Tracing::stop() {
    return stop_matching_owner(0, 0);
}

bool Tracing::active() {
    std::lock_guard<std::mutex> lk(g_mu);
    return static_cast<bool>(g_session);
}

TraceStopResult Tracing::stop_owned(const TraceOwnership& ownership) {
    if (ownership.generation_ == 0 || ownership.token_ == 0)
        return {};
    return stop_matching_owner(
        ownership.generation_, ownership.token_);
}

TraceOwnershipStatus Tracing::ownership_status(
    const TraceOwnership* ownership) {
    std::lock_guard<std::mutex> lk(g_mu);
    const bool active = static_cast<bool>(g_session);
    return {
        active,
        active && ownership &&
            ownership->generation_ != 0 &&
            ownership->token_ != 0 &&
            ownership->generation_ == g_session_generation &&
            ownership->token_ == g_ownership_token,
    };
}

std::uint64_t Tracing::session_generation() {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_session ? g_session_generation : 0;
}

int Tracing::attachment_count() {
    std::lock_guard<std::mutex> lk(g_attach_mu);
    return g_refcount;
}

void Tracing::attach() {
    // Serialize the whole 0→1/1→0 lifecycle. A new first attachment must not
    // observe an attach-owned session while the previous last detach is still
    // committed to stopping it.
    std::lock_guard<std::mutex> attach_lock(g_attach_mu);
    const bool first = g_refcount++ == 0;
    if (!first) return;

    // Env-driven autostart. A plug-in has no main() to call Tracing::start(),
    // so without this a PULP_TRACING=ON plug-in could never actually record —
    // the session API existed but nothing reached it. Setting PULP_TRACE_PATH
    // in the host's environment is the whole opt-in.
    const char* path = std::getenv("PULP_TRACE_PATH");
    if (!path || !*path || active()) return;
    auto started = start_exclusive({}, path);
    if (started.status != TraceStartStatus::Started ||
        !started.ownership)
        return;
    auto ownership = std::make_shared<const TraceOwnership>(
        std::move(*started.ownership));
    g_attach_ownership = ownership;

    // PULP_TRACE_SECONDS already caps the Perfetto buffer duration, but the
    // .pftrace is only written by stop(). Without a timed stop the file would
    // appear only when the last instance detaches, which for an editor session
    // means closing the plug-in — awkward when the thing being profiled IS the
    // open editor. Flush on a timer instead so the capture is self-completing.
    if (const char* s = std::getenv("PULP_TRACE_SECONDS"); s && *s) {
        if (int secs = std::atoi(s); secs > 0) {
            std::lock_guard<std::mutex> lk(g_timeout_mu);
            g_timeout.arm(
                std::chrono::seconds(secs), ownership->generation_,
                [ownership = std::move(ownership)](std::uint64_t armed_for) {
                    // Refuse to act on a session this timeout was not armed
                    // for. Close and reopen an editor inside the window and the
                    // old timer would otherwise truncate the new capture.
                    if (!detail::timeout_targets_current_session(
                            armed_for, Tracing::session_generation()))
                        return;
                    auto r = stop_owned(*ownership);
                    if (r.ok)
                        log_info("Tracing: auto-flushed {} bytes to {}",
                                 r.trace_bytes, r.path);
                });
        }
    }
}

void Tracing::detach() {
    // Last owner gone — cancel and JOIN the auto-flush timer, then tear the
    // session down. The join is the load-bearing part: this module can be
    // unloaded (FreeLibrary / dlclose) the moment the host is done with the
    // plug-in, and a timer thread that wakes after that runs freed code.
    //
    // Callers must ensure their audio callbacks are stopped before the final
    // detach (adapters call this from their destroy path, off the audio thread).
    std::lock_guard<std::mutex> attach_lock(g_attach_mu);
    if (g_refcount <= 0 || --g_refcount != 0)
        return;
    const auto ownership = std::exchange(
        g_attach_ownership,
        std::shared_ptr<const TraceOwnership>{});

    {
        std::lock_guard<std::mutex> lk(g_timeout_mu);
        g_timeout.cancel_and_join();
    }
    if (ownership)
        (void)stop_owned(*ownership);
}

}  // namespace pulp::runtime

#else  // PULP_TRACING_ENABLED — OFF: trivial stubs, no Perfetto.

namespace pulp::runtime {

bool Tracing::start(const std::vector<std::string>&, const std::string&,
                    std::uint32_t) {
    return false;
}
TraceStartResult Tracing::start_exclusive(
    const std::vector<std::string>&, const std::string&, std::uint32_t) {
    return {};
}
TraceStopResult Tracing::stop() { return {}; }
TraceStopResult Tracing::stop_owned(const TraceOwnership&) { return {}; }
TraceOwnershipStatus Tracing::ownership_status(
    const TraceOwnership*) { return {}; }
bool Tracing::active() { return false; }
void Tracing::attach() {}
void Tracing::detach() {}
std::uint64_t Tracing::session_generation() { return 0; }
int Tracing::attachment_count() { return 0; }

// Default shipping build: the reminder never fires — tracing is not compiled in.
void log_tracing_reminder() {}

}  // namespace pulp::runtime

#endif  // PULP_TRACING_ENABLED
