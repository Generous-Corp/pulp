// trace.cpp — process-global tracing controller (see trace_session.hpp).
//
// DEV ONLY. Under PULP_TRACING=OFF this compiles to trivial no-op stubs and
// pulls in zero Perfetto headers/symbols. Under ON it drives Perfetto's
// in-process backend: one process-wide session, call_once initialization
// (Initialize + Register are process singletons), reference-counted attachment
// for multi-instance hosts.

#include <pulp/runtime/trace_session.hpp>

#include <pulp/runtime/trace.hpp>  // PULP_TRACING_ENABLED + category storage decl

#if defined(PULP_TRACING_ENABLED) && PULP_TRACING_ENABLED

#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>

// Track-event storage for the categories declared in trace.hpp. Exactly one TU.
PERFETTO_TRACK_EVENT_STATIC_STORAGE();

namespace pulp::runtime {
namespace {

std::mutex g_mu;                                        // guards g_session/g_out
std::unique_ptr<perfetto::TracingSession> g_session;    // the one process session
std::string g_out_path;
std::atomic<int> g_refcount{0};
std::once_flag g_init_once;

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

}  // namespace

bool Tracing::start(const std::vector<std::string>& /*categories*/,
                    const std::string& out_path, std::uint32_t ring_kb) {
    ensure_initialized();
    std::lock_guard<std::mutex> lk(g_mu);
    if (g_session) return true;  // already active — one process session

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
    auto session = perfetto::Tracing::NewTrace();
    session->Setup(cfg);
    session->StartBlocking();
    g_session = std::move(session);
    return true;
}

TraceStopResult Tracing::stop() {
    std::unique_ptr<perfetto::TracingSession> session;
    std::string path;
    {
        std::lock_guard<std::mutex> lk(g_mu);
        if (!g_session) return {};
        session = std::move(g_session);
        path = g_out_path;
    }
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

bool Tracing::active() {
    std::lock_guard<std::mutex> lk(g_mu);
    return static_cast<bool>(g_session);
}

void Tracing::attach() { g_refcount.fetch_add(1, std::memory_order_relaxed); }

void Tracing::detach() {
    // Last owner gone — tear the session down (flush) if one is still active.
    // Callers must ensure their audio callbacks are stopped before the final
    // detach (adapters call this from their destroy path, off the audio thread).
    if (g_refcount.fetch_sub(1, std::memory_order_acq_rel) == 1) {
        if (active()) stop();
    }
}

}  // namespace pulp::runtime

#else  // PULP_TRACING_ENABLED — OFF: trivial stubs, no Perfetto.

namespace pulp::runtime {

bool Tracing::start(const std::vector<std::string>&, const std::string&,
                    std::uint32_t) {
    return false;
}
TraceStopResult Tracing::stop() { return {}; }
bool Tracing::active() { return false; }
void Tracing::attach() {}
void Tracing::detach() {}

}  // namespace pulp::runtime

#endif  // PULP_TRACING_ENABLED
