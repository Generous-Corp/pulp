#pragma once

/// @file exceptions.hpp
/// Portable try/catch macros for source that must also compile with C++
/// exceptions disabled (`-fno-exceptions`).
///
/// WHY THIS EXISTS: Pulp's WebAssembly plugin targets (WAMv2, WebCLAP) build
/// against the wasi-sdk threaded sysroot, whose libc++/libc++abi ship **without**
/// an exception-handling runtime (`__cxa_throw`, the Itanium personality, and the
/// unwinder are all absent). Code reaching those toolchains must compile under
/// `-fno-exceptions`, where the `try`/`catch` keywords are a hard compile error —
/// even in a function that is never called.
///
/// These macros let a *defensive* `try`/`catch(...)` block — one that swallows a
/// theoretical failure and falls through to a safe default — compile in both
/// modes with identical native behavior. With exceptions enabled they expand to a
/// real `try`/`catch`; with exceptions disabled the guarded block runs
/// unconditionally and the handler is dead-stripped.
///
/// IMPORTANT: Only use these where the catch is genuinely optional — i.e. the
/// guarded code is not expected to throw in practice and the handler just yields
/// a fallback. Do NOT use them to wrap control-flow that *relies* on catching a
/// thrown exception; under `-fno-exceptions` the throw aborts before the handler
/// runs, so the recovery never happens.

#if defined(__cpp_exceptions) && __cpp_exceptions

#define PULP_TRY        try
#define PULP_CATCH_ALL  catch (...)

#else

// Exceptions disabled: run the guarded block unconditionally, and make the
// handler a never-taken branch the optimizer removes. `if (false)` keeps the
// handler body syntactically valid (it is parsed, not discarded) without a
// `catch` keyword, which `-fno-exceptions` rejects.
//
// Only `catch (...)` is provided — a typed/bound handler (`catch (const E& e)`)
// cannot be expressed this way because its body would reference an undefined
// binding. Defensive sites that only need to swallow-and-fall-through use
// `catch (...)`, which is all these toolchains require.
#define PULP_TRY        if (true)
#define PULP_CATCH_ALL  if (false)

#endif
