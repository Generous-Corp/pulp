#pragma once

#include <cstdint>

namespace pulp::format {

/// Semantic role of a declared audio bus. This is static host metadata; it
/// does not change when a user switches a product's source mode.
enum class BusRole : std::uint8_t {
    Main,
    Aux,
    Sidechain,
};

} // namespace pulp::format
