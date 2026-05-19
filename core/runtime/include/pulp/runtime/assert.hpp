#pragma once

#include <pulp/platform/detect.hpp>
#include <source_location>
#include <string_view>
#include <cstdio>
#include <cstdlib>
#include <iostream>

namespace pulp::runtime {

[[noreturn]] inline void assert_fail(
    std::string_view expr,
    std::string_view msg,
    std::source_location loc = std::source_location::current())
{
    std::fprintf(stderr,
        "[pulp:ASSERT] %.*s\n"
        "  expression: %.*s\n"
        "  file: %s:%d\n"
        "  function: %s\n",
        static_cast<int>(msg.size()), msg.data(),
        static_cast<int>(expr.size()), expr.data(),
        loc.file_name(), loc.line(),
        loc.function_name());
    std::abort();
}

namespace detail {

// PULP_DBG_VAR helpers. Pre-stringified names + per-arg values come
// from the variadic macro below; here we just print them with a stable
// "[pulp:DBG] file:line | name = value | …" prefix.
template <typename T>
inline void dbg_var_one(std::string_view name, const T& value) {
    std::cerr << " | " << name << " = " << value;
}

inline void dbg_var_emit_prefix(std::source_location loc) {
    std::cerr << "[pulp:DBG] "
              << loc.file_name() << ':' << loc.line();
}

inline void dbg_var_emit_suffix() {
    std::cerr << '\n';
}

} // namespace detail

} // namespace pulp::runtime

// PULP_ASSERT: always checked (debug and release)
#define PULP_ASSERT(expr, msg) \
    do { \
        if (!(expr)) [[unlikely]] { \
            ::pulp::runtime::assert_fail(#expr, msg); \
        } \
    } while (false)

// PULP_DBG_ASSERT: checked only in debug builds
#if defined(NDEBUG)
    #define PULP_DBG_ASSERT(expr, msg) ((void)0)
#else
    #define PULP_DBG_ASSERT(expr, msg) PULP_ASSERT(expr, msg)
#endif

// PULP_DBG_VAR: name-printing debug print for ad-hoc inspection.
//
// Usage:
//   PULP_DBG_VAR(x);            // → [pulp:DBG] foo.cpp:42 | x = 7
//   PULP_DBG_VAR(x, y, z);      // → ... | x = 7 | y = 3.14 | z = "hi"
//
// Each argument is stringified once at the call site (`#x`) and the
// runtime value is streamed via `operator<<`. Compiles away to a
// no-op in NDEBUG builds — leave call sites in your code while you're
// chasing a bug, and they'll vanish when you flip to Release.
//
// Mirrors JUCE's `DBG_VAR` and Melatonin's `LOG_VAR` macros (sudara
// "Big List of JUCE Tips", #26). Replaces ad-hoc `std::cout << "x=" <<
// x` scaffolding.
#if defined(NDEBUG)
    #define PULP_DBG_VAR(...) ((void)0)
#else
    // Fold expression over the comma-separated arguments. Each one is
    // stringified at the call site by PULP_DBG_VAR_ONE and forwarded
    // to detail::dbg_var_one for value printing.
    #define PULP_DBG_VAR_ONE(x) ::pulp::runtime::detail::dbg_var_one(#x, (x))

    // Variadic version: comma-fold a single PULP_DBG_VAR_ONE per arg.
    // The do/while wraps the comma operator so the macro is statement-
    // safe inside `if`/`else` chains.
    #define PULP_DBG_VAR(...) \
        do { \
            ::pulp::runtime::detail::dbg_var_emit_prefix( \
                std::source_location::current()); \
            PULP_DBG_VAR_EXPAND(__VA_ARGS__) \
            ::pulp::runtime::detail::dbg_var_emit_suffix(); \
        } while (false)

    // Variadic expansion: this handles 1..8 args without needing
    // C++20 __VA_OPT__. Beyond 8 args, call PULP_DBG_VAR twice.
    #define PULP_DBG_VAR_NARGS_(_1,_2,_3,_4,_5,_6,_7,_8,N,...) N
    #define PULP_DBG_VAR_NARGS(...) \
        PULP_DBG_VAR_NARGS_(__VA_ARGS__, 8, 7, 6, 5, 4, 3, 2, 1)
    #define PULP_DBG_VAR_CAT_(a, b) a##b
    #define PULP_DBG_VAR_CAT(a, b) PULP_DBG_VAR_CAT_(a, b)
    #define PULP_DBG_VAR_EXPAND(...) \
        PULP_DBG_VAR_CAT(PULP_DBG_VAR_EXPAND_, PULP_DBG_VAR_NARGS(__VA_ARGS__))(__VA_ARGS__)
    #define PULP_DBG_VAR_EXPAND_1(x1) PULP_DBG_VAR_ONE(x1);
    #define PULP_DBG_VAR_EXPAND_2(x1, x2) PULP_DBG_VAR_ONE(x1); PULP_DBG_VAR_ONE(x2);
    #define PULP_DBG_VAR_EXPAND_3(x1, x2, x3) \
        PULP_DBG_VAR_ONE(x1); PULP_DBG_VAR_ONE(x2); PULP_DBG_VAR_ONE(x3);
    #define PULP_DBG_VAR_EXPAND_4(x1, x2, x3, x4) \
        PULP_DBG_VAR_ONE(x1); PULP_DBG_VAR_ONE(x2); \
        PULP_DBG_VAR_ONE(x3); PULP_DBG_VAR_ONE(x4);
    #define PULP_DBG_VAR_EXPAND_5(x1, x2, x3, x4, x5) \
        PULP_DBG_VAR_EXPAND_4(x1, x2, x3, x4) PULP_DBG_VAR_ONE(x5);
    #define PULP_DBG_VAR_EXPAND_6(x1, x2, x3, x4, x5, x6) \
        PULP_DBG_VAR_EXPAND_4(x1, x2, x3, x4) \
        PULP_DBG_VAR_ONE(x5); PULP_DBG_VAR_ONE(x6);
    #define PULP_DBG_VAR_EXPAND_7(x1, x2, x3, x4, x5, x6, x7) \
        PULP_DBG_VAR_EXPAND_4(x1, x2, x3, x4) \
        PULP_DBG_VAR_ONE(x5); PULP_DBG_VAR_ONE(x6); PULP_DBG_VAR_ONE(x7);
    #define PULP_DBG_VAR_EXPAND_8(x1, x2, x3, x4, x5, x6, x7, x8) \
        PULP_DBG_VAR_EXPAND_4(x1, x2, x3, x4) PULP_DBG_VAR_EXPAND_4(x5, x6, x7, x8)
#endif
