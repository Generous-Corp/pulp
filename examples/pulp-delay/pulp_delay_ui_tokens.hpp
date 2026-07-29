#pragma once

#include <pulp/canvas/canvas.hpp>

namespace pulp::examples::delay::ui {

namespace color {

inline constexpr canvas::Color background = canvas::Color::rgba8(14, 18, 24);
inline constexpr canvas::Color header = canvas::Color::rgba8(11, 15, 20);
inline constexpr canvas::Color panel = canvas::Color::rgba8(22, 28, 36);
inline constexpr canvas::Color panel_deep = canvas::Color::rgba8(17, 22, 29);
inline constexpr canvas::Color raised = canvas::Color::rgba8(29, 36, 45);
inline constexpr canvas::Color track = canvas::Color::rgba8(48, 58, 69);
inline constexpr canvas::Color border = canvas::Color::rgba8(43, 52, 63);
inline constexpr canvas::Color border_bright = canvas::Color::rgba8(62, 74, 87);
inline constexpr canvas::Color lime = canvas::Color::rgba8(183, 242, 52);
inline constexpr canvas::Color lime_soft = canvas::Color::rgba8(129, 174, 42);
inline constexpr canvas::Color lime_dim = canvas::Color::rgba8(66, 88, 35);
inline constexpr canvas::Color ink = canvas::Color::rgba8(237, 241, 235);
inline constexpr canvas::Color text = canvas::Color::rgba8(207, 215, 214);
inline constexpr canvas::Color muted = canvas::Color::rgba8(116, 128, 132);
inline constexpr canvas::Color faint = canvas::Color::rgba8(72, 82, 89);
inline constexpr canvas::Color black = canvas::Color::rgba8(8, 11, 14);

} // namespace color

namespace metric {

inline constexpr float editor_width = 1120.0f;
inline constexpr float editor_height = 740.0f;
inline constexpr float panel_radius = 8.0f;
inline constexpr float control_radius = 5.0f;

} // namespace metric

} // namespace pulp::examples::delay::ui
