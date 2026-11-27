#pragma once

/// @file legacy_view.hpp
/// `PULP_FORMAT_LEGACY_VIEW`: 1 (the default) while pulp-format-core's
/// headers still name Pulp's view layer for its create_view() editor; 0 in an
/// SDK whose UI comes from Vellum, set PUBLIC on pulp-format-core so every
/// consumer agrees. With 0, a format header that keeps a view-typed member for
/// layout declares that type opaque in `pulp::format::view` instead of naming
/// `pulp::view` (see processor.hpp for the reserved Processor slots).

#ifndef PULP_FORMAT_LEGACY_VIEW
#define PULP_FORMAT_LEGACY_VIEW 1
#endif
