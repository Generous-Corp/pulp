#pragma once

// SkSurfaceProps for a TextRasterPolicy. Every Skia surface Pulp creates for
// drawing goes through surface_props_for(), so the policy cannot reach one
// creation site and miss another.

#include <pulp/canvas/text_raster_policy.hpp>

#ifdef PULP_HAS_SKIA

#include "include/core/SkSurfaceProps.h"

#include <cstdint>

namespace pulp::canvas {

/// The props a surface created under `policy` carries. `legacy` returns the
/// default-constructed props, which is exactly what a surface created without
/// props gets, so passing it changes nothing.
inline SkSurfaceProps surface_props_for(TextRasterPolicy policy, std::uint32_t flags = 0,
                                        SkPixelGeometry geometry = kUnknown_SkPixelGeometry) {
    if (policy == TextRasterPolicy::legacy && flags == 0 && geometry == kUnknown_SkPixelGeometry)
        return SkSurfaceProps();
    const TextRasterTuple tuple = text_raster_tuple(policy);
    return SkSurfaceProps(flags, geometry, tuple.contrast, tuple.gamma);
}

/// The policy whose tuple `props` carries, so a surface derived from another
/// (a layer of the frame target) can be checked against its parent.
inline TextRasterPolicy text_raster_policy_of(const SkSurfaceProps& props) {
    return props.textContrast() == kChromiumTextRaster.contrast &&
                   props.textGamma() == kChromiumTextRaster.gamma
               ? TextRasterPolicy::chromium
               : TextRasterPolicy::legacy;
}

} // namespace pulp::canvas

#endif // PULP_HAS_SKIA
