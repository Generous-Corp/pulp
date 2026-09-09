#pragma once

#include <pulp/format/detail/standalone_key_sequence.hpp>

namespace pulp::format::detail {

/// True when this build can synthesize native key events for the harness.
bool synthetic_key_delivery_supported();

/// Deliver one synthesized key press (down then up) to `native_content_view`,
/// which is the value of WindowHost::native_content_view_handle().
///
/// The delivery reproduces the host's REAL key path, including the order the
/// window manager offers the event in. Returns false when the platform has no
/// synthetic-key support or the view handle is null.
bool deliver_synthetic_key(void* native_content_view, const KeySequenceStep& step);

}  // namespace pulp::format::detail
