#pragma once

// A begin/end trace-span stack the script side drives.
//
// A C++ scope span is balanced by lifetime; a script-driven one is not. The
// script opens a span in one native call and closes it in another, so nothing
// but the script's own control flow keeps the pair balanced, and an unbalanced
// pair does not announce itself: it silently re-parents every later slice under
// a span that never closed, which corrupts attribution downstream instead of
// failing. So the dispatch boundary force-closes whatever a handler left open
// AND publishes that force-close — a counter, a named slice, and a line on
// stderr — rather than repairing it quietly.
//
// The depth bookkeeping runs in every configuration, tracing on or off, so the
// balance counters below are a deterministic test seam on the default build
// where the Perfetto macros compile to nothing.

#include <pulp/runtime/trace.hpp>

#include <iostream>
#include <string>

namespace pulp::view::js_trace {

// Deepest script-opened nesting one handler may hold. A script that only ever
// calls begin() would otherwise grow the span stack without bound; past the cap
// begin() opens nothing and says so in its return value and in the stats.
inline constexpr int max_depth = 64;

namespace detail {
inline int& depth()             { static thread_local int d = 0;        return d; }
inline long long& refused()     { static thread_local long long n = 0;  return n; }
inline long long& force_closed(){ static thread_local long long n = 0;  return n; }
inline long long& unmatched()   { static thread_local long long n = 0;  return n; }
} // namespace detail

/// Opens a `js` span named `name`. Returns false when the depth cap refused it,
/// in which case nothing was opened and the caller must not expect a close.
inline bool begin(const std::string& name) {
    if (detail::depth() >= max_depth) {
        ++detail::refused();
        return false;
    }
    PULP_TRACE_BEGIN_DYNAMIC("js", name);
    ++detail::depth();
    return true;
}

/// Closes the innermost script-opened span. Returns false when the script
/// closed more than it opened; that is counted, because it is the same defect
/// class as leaving one open.
inline bool end() {
    if (detail::depth() <= 0) {
        ++detail::unmatched();
        return false;
    }
    --detail::depth();
    PULP_TRACE_END("js");
    return true;
}

/// Closes every span still open on this thread and returns how many there were.
/// A non-zero return is a defect in the script, so it is made observable rather
/// than absorbed.
inline int force_close_open_scopes(const char* context) {
    const int leaked = detail::depth();
    if (leaked <= 0) return 0;
    for (int i = 0; i < leaked; ++i) PULP_TRACE_END("js");
    detail::depth() = 0;
    detail::force_closed() += leaked;
    PULP_TRACE_COUNTER("js", "js_trace_unbalanced_scopes", leaked);
    { PULP_TRACE_SCOPE_NAMED("js", "js_trace_force_closed_unbalanced_scope"); }
    std::cerr << "WidgetBridge " << (context ? context : "js trace")
              << ": script left " << leaked
              << " __traceBegin__ scope(s) open; force-closed\n";
    return leaked;
}

/// Spans open on this thread right now.
inline int open_depth() { return detail::depth(); }
/// Spans force-closed at a dispatch boundary since process start.
inline long long force_closed_count() { return detail::force_closed(); }
/// __traceEnd__ calls that had nothing to close.
inline long long unmatched_end_count() { return detail::unmatched(); }
/// __traceBegin__ calls refused by the depth cap.
inline long long refused_count() { return detail::refused(); }

} // namespace pulp::view::js_trace
