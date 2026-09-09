#include <pulp/format/detail/standalone_key_driver.hpp>

// Non-Apple standalone hosts have no synthetic-key backend yet. Reporting
// unsupported (rather than silently succeeding) lets the harness log a real
// reason instead of a run that presses nothing and still looks green.

namespace pulp::format::detail {

bool synthetic_key_delivery_supported() { return false; }

bool deliver_synthetic_key(void*, const KeySequenceStep&) { return false; }

}  // namespace pulp::format::detail
