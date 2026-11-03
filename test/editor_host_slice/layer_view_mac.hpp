#pragma once

// A minimal NSView that shows premultiplied RGBA frames, for editors that
// render on the CPU. Handles are `NSView*` passed as `void*`.

#include <cstdint>

namespace pulp::slice {

void* layer_view_create(void* parent_ns_view, std::uint32_t width, std::uint32_t height);
void layer_view_resize(void* view, std::uint32_t width, std::uint32_t height);
void layer_view_update(void* view, const std::uint8_t* premultiplied_rgba, std::uint32_t width,
                       std::uint32_t height);
/// Remove from the parent and release.
void layer_view_destroy(void* view);
/// Release without touching the parent (it is already gone).
void layer_view_abandon(void* view);
/// Subviews `parent_ns_view` currently has, for teardown checks.
int layer_view_subview_count(void* parent_ns_view);

} // namespace pulp::slice
