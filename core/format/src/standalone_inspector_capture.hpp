#pragma once

#include <pulp/inspect/capture_source.hpp>

namespace pulp::view {
class View;
class WindowHost;
} // namespace pulp::view

namespace pulp::format {
class Processor;

namespace detail {

bool standalone_capture_available(view::View& root, const view::WindowHost& window);
bool standalone_capture_producer_available(const view::WindowHost& window);
inspect::InspectorCapture capture_standalone_png(view::View& root, view::WindowHost& window,
                                                 Processor& processor);

} // namespace detail
} // namespace pulp::format
