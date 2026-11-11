// SPDX-License-Identifier: MIT
#pragma once

#include <choc/containers/choc_Value.h>
#include <string>
#include <string_view>

namespace pulp::host {
struct PreparedDescriptionData;

namespace detail {
// The caller owns the canonical encoder and the public Result exception boundary.
// Validate before projection, because projection can otherwise erase duplicate
// physical classes or unused bindings supplied in a detached capture.
bool validate_resolved_identity_capture(const PreparedDescriptionData&);
bool validate_resolved_identity_projection(choc::value::ValueView projection,
                                           std::string_view authored_canonical_payload,
                                           std::string_view authored_digest,
                                           std::string (*canonical_encode)(choc::value::ValueView));
} // namespace detail
} // namespace pulp::host
