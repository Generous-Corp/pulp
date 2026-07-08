// trace.hpp — Pulp tracing macros (Perfetto-backed, DEV ONLY, OFF by default).
//
// This is the ONLY header instrumentation code includes for tracing. With
// PULP_TRACING=OFF — the default and only shippable configuration — every macro
// compiles to nothing and NO Perfetto header is pulled in: zero cost, zero
// symbols (see tools/cmake/PulpTracing.cmake and the guards it names).
//
// RT-safety: D1 (planning/2026-07-08-perfetto-tracing-plan.md §0c) proved
// Perfetto's TRACE_EVENT is NOT real-time-safe — it locks a mutex on chunk
// rollover. These macros must NEVER be placed on the audio thread's live
// process() path. They are for UI / render / process-level and OFFLINE-audio
// spans only; live DSP observability uses the fixed-slot per-node telemetry
// fallback instead.
#pragma once

namespace pulp::runtime {

/// True only in a PULP_TRACING=ON build. Every shipping/default build is false.
#if defined(PULP_TRACING_ENABLED) && PULP_TRACING_ENABLED
inline constexpr bool kTracingEnabled = true;
#else
inline constexpr bool kTracingEnabled = false;
#endif

}  // namespace pulp::runtime

#if defined(PULP_TRACING_ENABLED) && PULP_TRACING_ENABLED

#include <array>
#include <string_view>

#include "perfetto.h"

// Category taxonomy — the fixed query vocabulary. Declared here (storage in
// trace.cpp via PERFETTO_TRACK_EVENT_STATIC_STORAGE). `dsp`/`dsp.node` are used
// only from OFFLINE renders, never live process() (D1).
PERFETTO_DEFINE_CATEGORIES(
    perfetto::Category("dsp").SetDescription("Offline DSP block timing"),
    perfetto::Category("dsp.node").SetDescription("Offline per-node DSP timing"),
    perfetto::Category("render").SetDescription("Frame render pipeline"),
    perfetto::Category("layout").SetDescription("Yoga layout passes"),
    perfetto::Category("canvas").SetDescription("Canvas 2D drawing"),
    perfetto::Category("text").SetDescription("Text shaping (prepare vs layout)"),
    perfetto::Category("js").SetDescription("QuickJS bridge dispatch"),
    perfetto::Category("gpu").SetDescription("Dawn/Graphite GPU passes"),
    perfetto::Category("state").SetDescription("Parameter/state changes"),
    perfetto::Category("io").SetDescription("File / network I/O"));

namespace pulp::runtime::trace_detail {

// Compile-time PRETTY_FUNCTION → "Class::method" trimmer.
//
// Ported (adapted) from sudara/melatonin_perfetto (MIT) — the one genuinely
// clever artifact worth copying: it keeps the trimmed name a compile-time value
// so the span name stays a perfetto::StaticString with zero runtime formatting.
// JUCE-isms dropped; behavior preserved. See NOTICE.md (melatonin_perfetto).
//
// The lambda-wrapped-string trick (accu.org/journals/overload/30/172/wu) lets a
// string be a template parameter so trimming happens at compile time.
#define PULP_TRACE_WRAP_CT_STRING(x) [] { return (x); }
#define PULP_TRACE_UNWRAP_CT_STRING(x) (x)()

template <typename WrappedString>
constexpr auto prettify_function(WrappedString wrapped) {
    constexpr auto src = PULP_TRACE_UNWRAP_CT_STRING(wrapped);
    constexpr auto size = std::string_view(src).size();
    std::array<char, size> result{};

    for (size_t i = 0; i < size; ++i) {
        // Skip the return type (everything up to the first space).
        if (src[i] == ' ') {
            ++i;
            // MSVC emits an extra "__cdecl " after the return type.
            if (src[i + 1] == '_') i += 8;

            size_t j = 0;
            // Copy the qualified name; stop at the argument list. clang/gcc use
            // '(', MSVC uses '<' (the lambda identifier).
            while ((src[i] != '(' && src[i] != '<') && i < size && j < size) {
                result[j] = src[i];
                ++i;
                ++j;
            }
            // MSVC clean-up: strip the trailing "::" before "<lambda_1>".
            if (src[i] == '<') result[j - 2] = '\0';
            return result;
        }
    }
    return result;
}

// Compile-time self-tests for the trimmer (mirrors the melatonin battery). If
// any fires, the trimmer regressed on this compiler's PRETTY_FUNCTION shape.
namespace prettify_test {
template <size_t N, size_t M>
constexpr bool eq(const std::array<char, N>& r, const char (&t)[M]) {
    static_assert(M > 1);
    static_assert(N + 1 >= M);
    return std::string_view(t, M) == std::string_view(r.data(), M);
}
static_assert(eq(prettify_function(PULP_TRACE_WRAP_CT_STRING("int main")), "main"));
static_assert(eq(prettify_function(PULP_TRACE_WRAP_CT_STRING(
                     "void Foo::bar(int, float)::(anonymous class)::operator()()")),
                 "Foo::bar"));
}  // namespace prettify_test

}  // namespace pulp::runtime::trace_detail

// Public macro surface. `category` is one of the taxonomy string literals.
// Auto-named spans use the enclosing function's prettified name. The span name
// is always a perfetto::StaticString, so a dynamic std::string does NOT compile
// — the compile-time ban on RT-unsafe dynamic names (plan §0b).
#define PULP_TRACE_SCOPE(category)                                                     \
    static constexpr auto _pulp_tr_pf =                                                \
        ::pulp::runtime::trace_detail::prettify_function(                              \
            PULP_TRACE_WRAP_CT_STRING(PERFETTO_DEBUG_FUNCTION_IDENTIFIER()));          \
    TRACE_EVENT(category, ::perfetto::StaticString(_pulp_tr_pf.data()))

#define PULP_TRACE_SCOPE_NAMED(category, name) \
    TRACE_EVENT(category, ::perfetto::StaticString(name))

// A named scope span carrying typed debug-annotation args as trailing
// "key", value pairs — e.g.
//   PULP_TRACE_SCOPE_NAMED_ARGS("dsp", "offline_block",
//                               "block_index", idx, "position_samples", pos);
// Each key must be a string literal; Perfetto stores it as arg
// `debug.<key>`, so SQL reads it via EXTRACT_ARG(arg_set_id, 'debug.<key>').
#define PULP_TRACE_SCOPE_NAMED_ARGS(category, name, ...) \
    TRACE_EVENT(category, ::perfetto::StaticString(name), __VA_ARGS__)

#define PULP_TRACE_BEGIN(category, name) \
    TRACE_EVENT_BEGIN(category, ::perfetto::StaticString(name))

#define PULP_TRACE_END(category) TRACE_EVENT_END(category)

#define PULP_TRACE_COUNTER(category, name, value) \
    TRACE_COUNTER(category, ::perfetto::StaticString(name), (value))

#else  // PULP_TRACING_ENABLED

// OFF: names exist as no-ops in every configuration so instrumentation sites
// compile and cost nothing. No Perfetto header is pulled in.
#define PULP_TRACE_SCOPE(category) ((void)0)
#define PULP_TRACE_SCOPE_NAMED(category, name) ((void)0)
#define PULP_TRACE_SCOPE_NAMED_ARGS(category, name, ...) ((void)0)
#define PULP_TRACE_BEGIN(category, name) ((void)0)
#define PULP_TRACE_END(category) ((void)0)
#define PULP_TRACE_COUNTER(category, name, value) ((void)0)

#endif  // PULP_TRACING_ENABLED
