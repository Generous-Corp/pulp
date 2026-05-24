#pragma once

// Shared SkUnicode singleton for Skia m149+ ParagraphBuilder.
//
// Skia m149 retired the 2-arg `ParagraphBuilder::make(style, fontCollection)`
// overload in favor of the 3-arg form that requires `sk_sp<SkUnicode>`.
// Pulp constructs one process-wide ICU-backed SkUnicode here, lazily on
// first call, and shares the `sk_sp` ref across every paragraph builder.
//
// Lifetime: the function-local `static sk_sp<SkUnicode>` lives until
// process exit. AU v3 .appex extensions are XPC processes whose lifetime
// equals the framework's load lifetime, so a single instance is safe and
// concurrent editor opens (Logic hosting multiple AU instances of the
// same plugin) share the same singleton without re-init or teardown
// races. `sk_sp` copy is a cheap ref-count bump on each call.

#include "modules/skunicode/include/SkUnicode_icu.h"

namespace pulp::canvas {

inline sk_sp<SkUnicode> shared_sk_unicode() {
    static sk_sp<SkUnicode> unicode = SkUnicodes::ICU::Make();
    return unicode;
}

}  // namespace pulp::canvas
