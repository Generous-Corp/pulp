#pragma once

/// @file slew_limiter.hpp
/// Compatibility include for the canonical modulation-toolkit slew limiter
/// and sample-and-hold implementations.
///
/// The implementations live in `mod_tools.hpp` so including both public
/// headers cannot create competing definitions or include-order-dependent
/// behaviour.

#include <pulp/signal/mod_tools.hpp>
