#pragma once

#include <string>

namespace pulp::canvas {
class Canvas;
}

namespace pulp::view {

class View;

// Interprets a declarative JSON widget definition and paints it onto the given
// canvas. `w`/`h` are the widget bounds; `value` is the normalized (0..1)
// parameter value bound to angle/line elements. On invalid JSON it paints an
// error indicator rectangle. Moved verbatim out of widgets.cpp.
void render_schema(canvas::Canvas& canvas, const std::string& json,
                   float w, float h, float value, View& view);

}  // namespace pulp::view
