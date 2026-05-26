#pragma once

// VST3 SMPTE frame-rate → pulp::format::FrameRate mapping.
//
// Extracted from vst3_adapter.cpp so the table is unit-testable without
// pulling the Steinberg VST3 SDK into the test binary. The VST3 host
// passes `framesPerSecond` (integer) plus a flags bitfield containing
// kPullDownRate and kDropRate; we collapse them into Pulp's enum.
//
// Reference: ivstprocesscontext.h `FrameRate` comment block. VST3 docs:
//   23.976  = 24 + pulldown
//   29.97   = 30 + pulldown
//   29.97-drop = 30 + pulldown + drop
//   30-drop = 30 + drop
//   59.94   = 60 + pulldown        ← NOT true fps_60
//   integer rates have flags == 0
//
// Pulp's FrameRate enum currently lists only the seven rates the spec
// covers as engineering primitives (24/25/29.97/29.97-drop/30/30-drop/60).
// Other rates (50, 59.94) map to FrameRate::unknown rather than the
// nearest neighbour, because mislabelling 59.94 as fps_60 breaks SMPTE
// math in downstream plug-ins.

#include <pulp/format/processor.hpp>

namespace pulp::format::detail {

/// Map a VST3 SMPTE quad (fps, pulldown, drop) → Pulp FrameRate.
///
/// `fps` is the integer `framesPerSecond` field; `pulldown` and `drop`
/// are the kPullDownRate / kDropRate flag bits from `FrameRate::flags`.
/// Anything outside the seven enumerated rates returns FrameRate::unknown.
inline pulp::format::FrameRate vst3_frame_rate(int fps,
                                                bool pulldown,
                                                bool drop) noexcept {
    using FR = pulp::format::FrameRate;
    if (fps == 24) return FR::fps_24;  // 23.976 (=24+pulldown) ≈ 24 in our enum
    if (fps == 25 && !pulldown && !drop) return FR::fps_25;
    if (fps == 30 && pulldown && drop) return FR::fps_29_97_drop;
    if (fps == 30 && pulldown) return FR::fps_29_97;
    if (fps == 30 && drop) return FR::fps_30_drop;
    if (fps == 30) return FR::fps_30;
    if (fps == 60 && !pulldown) return FR::fps_60;
    // fps==60 + pulldown is 59.94 — has no enum entry, fall through.
    return FR::unknown;
}

}  // namespace pulp::format::detail
