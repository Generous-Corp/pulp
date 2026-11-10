#pragma once

#include <pulp/host/custom_node_type.hpp>

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>
#include <vector>

namespace pulp::host {

inline constexpr std::string_view kControlIndexInputTypeId = "pulp.control.index-input";
inline constexpr int kControlIndexInputVersion = 1;
inline constexpr std::size_t kControlIndexMaxFrames = 4096;

/// Registered graph-owned source for an external per-sample table index.
CustomNodeType make_control_index_input_type();

CustomNodeType make_control_min_type();
CustomNodeType make_control_max_type();
CustomNodeType make_control_safe_divide_type();
CustomNodeType make_control_map_clip_type();
CustomNodeType make_control_hysteresis_type();
CustomNodeType make_control_integrator_type();
CustomNodeType make_control_slew_type();
CustomNodeType make_control_indexed_table_type();

/// Versioned state encoders for set_custom_node_state(). Scalar value order:
/// map: input min/max, output min/max; hysteresis: high, low;
/// integrator: min, max, initial; slew: rise ms, fall ms.
std::vector<std::uint8_t> make_control_scalar_state(std::span<const float> values);
std::vector<std::uint8_t> make_control_table_state(std::span<const float> samples);
bool validate_control_node_state(std::string_view type_id, const std::vector<std::uint8_t>& bytes);

namespace detail {
struct ControlIndexInputState {
    std::array<float, kControlIndexMaxFrames> indices{};
    std::size_t frames = 0;
};
} // namespace detail

} // namespace pulp::host
