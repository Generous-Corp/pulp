#pragma once

// The harness's stand-in for a host window: a real, never-shown NSWindow whose
// content view is the editor's parent.

#include <cstdint>

namespace pulp::slice {

void host_app_init();
void* host_window_create(std::uint32_t width, std::uint32_t height);
void* host_window_content_view(void* window);
void host_window_resize(void* window, std::uint32_t width, std::uint32_t height);
void host_window_destroy(void* window);
int host_view_subview_count(void* view);
/// Drain pending AppKit events and run the main run loop briefly.
void host_pump_events();

} // namespace pulp::slice
