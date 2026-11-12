// Compiles vellum_ui/mirror_spelling.hpp against a stand-in SDK include tree and
// checks that it chose that tree's spelling. SLICE_EXPECT_VELLUM is 1 for a
// <vellum/...> tree and 0 for a <pulp/...> one.

#include "mirror_spelling.hpp"

namespace units = slice_vellum_ui::units;

static_assert(slice_vellum_ui::units_spelled_vellum == (SLICE_EXPECT_VELLUM != 0),
              "mirror_spelling.hpp chose the wrong spelling for this SDK layout");
static_assert(units::canvas::Canvas::spelled_vellum == (SLICE_EXPECT_VELLUM != 0),
              "slice_vellum_ui::units names the other spelling's namespace");
static_assert(sizeof(units::view::View) > 0, "the view unit resolves through the alias");

int main() {
    return 0;
}
