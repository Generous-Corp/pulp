#pragma once

/// @file aliases.hpp
/// The Vellum view types the components in this package build on, named once
/// so each component header reads like the widget it describes.

#include <vellum/canvas/canvas.hpp>
#include <vellum/view/buttons.hpp>
#include <vellum/view/context_menu.hpp>
#include <vellum/view/input_events.hpp>
#include <vellum/view/view.hpp>
#include <vellum/view/widgets.hpp>

namespace pulp::vellum_components {

namespace canvas = ::vellum::canvas;
namespace view = ::vellum::view;
using ::vellum::view::ContextMenu;
using ::vellum::view::FlexAlign;
using ::vellum::view::FlexDirection;
using ::vellum::view::KeyCode;
using ::vellum::view::KeyEvent;
using ::vellum::view::Label;
using ::vellum::view::MouseButton;
using ::vellum::view::MouseEvent;
using ::vellum::view::Point;
using ::vellum::view::TextButton;
using ::vellum::view::ToggleButton;
using ::vellum::view::View;

} // namespace pulp::vellum_components
