#pragma once

/// @file forge_compressor_ballistics.hpp
/// The "ballistics" control shared by the Forge nodes built on
/// signal::Compressor (lo-fi, sidechain and multiband compressors).

#include <pulp/host/forge_param_descriptor.hpp>
#include <pulp/signal/compressor.hpp>

#include <string_view>

namespace pulp::host::forge_compressor {

/// The ballistics a baked value selects. The nodes declare the control as
/// 0..1 with default 0, which is branching (the compressor's default, so a
/// patch saved before the control existed renders as before); anything from
/// 0.5 up is decoupled.
inline constexpr signal::CompressorBallistics ballistics_for(float value) noexcept {
    return value >= 0.5f ? signal::CompressorBallistics::decoupled
                         : signal::CompressorBallistics::branching;
}

/// The descriptor entry, given the node's own stable parameter id.
inline ForgeParamDescriptor ballistics_descriptor(state::ParamID id) {
    return {"ballistics",
            id,
            "Ballistics",
            "",
            "How gain reduction follows the level. Branching is the classic one-pole "
            "detector; decoupled holds each peak, so attack follows its setting on "
            "sustained tones and low notes distort less.",
            ForgeParamKind::stepped,
            ForgeParamCurve::linear,
            {{"branching", "Branching", 0.0f}, {"decoupled", "Decoupled", 1.0f}}};
}

} // namespace pulp::host::forge_compressor
