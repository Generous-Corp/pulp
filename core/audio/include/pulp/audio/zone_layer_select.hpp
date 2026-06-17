#pragma once

/// @file zone_layer_select.hpp
/// Multi-layer (multi-mic / velocity-layer) selection over a SampleZoneMap.
///
/// ZoneSelector::select() returns the single best zone for a trigger. A
/// multisample instrument often needs several zones to sound at once: multiple
/// microphone positions, or overlapping velocity layers that crossfade. This
/// helper returns every zone that should sound for one (note, velocity)
/// trigger, while still rotating round-robin variations within each non-zero
/// round_robin_group (so RR alternates are not all triggered together).
///
/// Selection rules:
///   * A zone must match the request (key + velocity), same as
///     ZoneSelector::matches().
///   * round_robin_group == 0 zones are independent layers — all matching ones
///     are emitted (this is the multi-mic / velocity-layer case).
///   * round_robin_group != 0 zones are rotation variants — only the variant
///     chosen by request.round_robin_step is emitted per group.
///
/// RT-safe when the map is immutable for the duration of the call: no
/// allocation, no locking. O(zones^2) in the worst case, which is negligible
/// for realistic zone counts.

#include <cstddef>

#include <pulp/audio/sample_zone_map.hpp>

namespace pulp::audio {

/// Resolve a trigger to every zone that should sound. Writes up to @p max_out
/// ZoneSelection entries (each with resolved pitch / playback rate) into
/// @p out and returns the count written. Extra matches beyond @p max_out are
/// dropped.
std::size_t select_layers(const SampleZoneMap& map,
                          const ZoneSelectionRequest& request,
                          ZoneSelection* out, std::size_t max_out) noexcept;

}  // namespace pulp::audio
