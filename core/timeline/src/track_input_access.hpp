#pragma once

#include <pulp/timeline/model.hpp>

namespace pulp::timeline::detail {

/// Returns the complete authored input `track` was built from.
///
/// An identity rewrite copies this and replaces only the identity-bearing
/// fields, so authored value state — name, arm intent, gain, pan — is carried
/// across by construction rather than by each rewrite site remembering to name
/// it. Adding a member to TrackInput means adding it here, next to the storage
/// it reads.
TrackInput track_input_of(const Track& track);

} // namespace pulp::timeline::detail
