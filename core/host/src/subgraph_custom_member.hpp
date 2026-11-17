#pragma once

#include <pulp/host/custom_node_type.hpp>

#include <cstdint>
#include <vector>

namespace pulp::host::detail {

// Whether `state` is opaque state a Custom subgraph member of `type` can
// load. Empty state is always accepted. Non-empty state needs the type's
// create and load_state: a probe instance is created, handed the bytes, and
// destroyed again, all on the calling (control) thread, so a document or
// template cannot carry state the type refuses until the first prepare().
// A throwing create, load_state or destroy counts as a refusal; the probe
// instance is destroyed whenever it was created.
bool custom_member_state_accepted(const CustomNodeType& type,
                                  const std::vector<std::uint8_t>& state) noexcept;

} // namespace pulp::host::detail
