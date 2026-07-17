#pragma once

#include <pulp/view/script_engine.hpp>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

namespace pulp::view {

// A JSON-style JS string literal (double-quoted, fully escaped) for `text`,
// safe to splice into evaluated JS wherever a string argument is expected.
std::string js_string_literal(std::string_view text);

// Evaluate `js` on the engine, then pump the microtask loop so React setState
// (and any queued Promise/microtask continuations) commit before the next event
// arrives. Exceptions are swallowed and logged to stderr tagged with `context`.
//
// The alive-flag overload is a no-op when the bridge has been torn down (the
// alive flag is cleared) or the engine is null/invalid; the no-flag overload
// always runs and targets a known-valid engine reference.
void safe_dispatch_eval(const std::shared_ptr<std::atomic<bool>>& alive,
                        ScriptEngine* engine,
                        const std::string& js,
                        const char* context);
void safe_dispatch_eval(ScriptEngine& engine, const std::string& js, const char* context);

// Dispatch a DOM-style event into the JS bridge, i.e.
// `__dispatch__(<id>, '<event_name>', <payload_expr>)`. The target `id` is
// ALWAYS routed through js_string_literal, so an id containing a quote or
// backslash cannot break out of the string literal. `event_name` is a fixed
// internal token spliced verbatim; `payload_expr` is a ready-to-eval JS
// expression. The event name doubles as the stderr error context.
void dispatch_event(const std::shared_ptr<std::atomic<bool>>& alive,
                    ScriptEngine* engine,
                    std::string_view id,
                    const std::string& event_name,
                    std::string_view payload_expr);
void dispatch_event(ScriptEngine& engine,
                    std::string_view id,
                    const std::string& event_name,
                    std::string_view payload_expr);

} // namespace pulp::view
