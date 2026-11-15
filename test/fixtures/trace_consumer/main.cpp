// Emits trace events from consumer code built against an installed Pulp SDK.
//
// The session is started and stopped through the public controller, the way a
// test hook or a plug-in would do it, rather than through PULP_TRACE_PATH, so
// the check covers the programmatic API as well as the macros. The same source
// compiles against an untraced SDK: every call then reports "unavailable" and
// no file is written.
//
// Usage: trace-consumer <output.pftrace>
// Prints one machine-readable status line and exits 0 when it ran to the end.

#include <pulp/runtime/trace.hpp>
#include <pulp/runtime/trace_session.hpp>

#include <cstdio>
#include <string>

namespace {

volatile unsigned g_sink = 0;

// Some work for the span to cover, so its duration is not zero.
void consumer_work() {
    PULP_TRACE_SCOPE_NAMED("lifecycle", "consumer_probe");
    unsigned acc = 0;
    for (unsigned i = 0; i < 200000; ++i)
        acc = acc * 1664525u + 1013904223u;
    g_sink = acc;
}

} // namespace

int main(int argc, char** argv) {
    if (argc != 2) {
        std::fprintf(stderr, "usage: trace-consumer <output.pftrace>\n");
        return 64;
    }
    using pulp::runtime::TraceStartStatus;
    using pulp::runtime::Tracing;

    auto started = Tracing::start_exclusive({"lifecycle"}, argv[1], 4u * 1024u);
    const bool owned = started.status == TraceStartStatus::Started && started.ownership;

    consumer_work();
    PULP_TRACE_INSTANT_ARGS("lifecycle", "consumer_probe_instant", "frame_index", 7);

    pulp::runtime::TraceStopResult stopped;
    if (owned)
        stopped = Tracing::stop_owned(*started.ownership);

    const char* status = started.status == TraceStartStatus::Started         ? "started"
                         : started.status == TraceStartStatus::AlreadyActive ? "already_active"
                                                                             : "unavailable";
    std::printf("trace-consumer: compiled_in=%d start=%s stopped=%d path=%s bytes=%llu\n",
                pulp::runtime::kTracingEnabled ? 1 : 0, status, stopped.ok ? 1 : 0,
                stopped.path.empty() ? "-" : stopped.path.c_str(),
                static_cast<unsigned long long>(stopped.trace_bytes));
    // A session this process started and could not flush is a real failure;
    // everything else is for the driver to judge.
    return (owned && !stopped.ok) ? 1 : 0;
}
