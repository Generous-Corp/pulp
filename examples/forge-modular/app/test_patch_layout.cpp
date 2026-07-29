// Corpus for the preview's geometry.
//
// The painting either works or obviously does not. The geometry is where a
// preview goes subtly wrong: a cable on the wrong jack, a panel at the wrong
// width shifting everything after it, an unmapped module drawn as though we
// knew where its ports were. Each of those looks entirely plausible on screen,
// which is exactly why it needs asserting rather than eyeballing.
//
// Real numbers throughout, measured from a running Rack: Fundamental's VCO is
// 135 px wide with input 0 at 0.3852 of that, and its input 1 sits to the LEFT
// of input 0 -- index order is not visual order, and a layout that assumed
// otherwise would put cables in the wrong holes while looking fine.

#include "patch_layout.hpp"

#include <cmath>
#include <cstdio>
#include <string>

namespace {

int failures = 0;

void check(bool ok, const std::string& what) {
    std::printf(ok ? "  ok     %s\n" : "  WRONG  %s\n", what.c_str());
    if (!ok) ++failures;
}

bool near(float a, float b, float tol = 0.01f) { return std::fabs(a - b) < tol; }

forge_modular::LayoutModule vco() {
    forge_modular::LayoutModule m;
    m.slug = "Fundamental/VCO";
    m.hp = 9;
    m.width = 135.f;
    m.image = "/tmp/VCO.png";
    m.mapped = true;
    // As recorded: input 1 is to the LEFT of input 0.
    m.ports = {{0, true, "1V/octave pitch", 0.3852f, 286.f},
               {1, true, "Frequency modulation", 0.1481f, 286.f},
               {2, false, "Sawtooth", 0.6185f, 334.f}};
    return m;
}

forge_modular::LayoutModule vca_unmapped() {
    forge_modular::LayoutModule m;
    m.slug = "Fundamental/VCA";
    m.hp = 5;
    m.width = 75.f;
    m.image = "";        // never rendered
    m.mapped = false;    // never placed, so no port positions
    return m;
}

}  // namespace

int main() {
    std::map<std::string, forge_modular::LayoutModule> catalog{
        {"Fundamental/VCO", vco()}, {"Fundamental/VCA", vca_unmapped()}};

    forge_modular::PatchLayout L;
    L.set_catalog(catalog);
    // Deliberately out of order, and with one module nobody has heard of.
    L.place({{3, "Nobody/Whatever", 30.f},
             {1, "Fundamental/VCO", 10.f},
             {2, "Fundamental/VCA", 20.f}});

    const auto& mods = L.modules();
    check(mods.size() == 3, "every module is placed");
    check(mods[0].id == 1 && mods[1].id == 2 && mods[2].id == 3,
          "sorted into the patch's own left-to-right order");

    check(near(mods[0].x, 0.f), "the first panel starts at the left edge");
    check(near(mods[1].x, 135.f), "the second starts where the first ends");
    check(near(mods[2].x, 210.f), "and the third after that");
    check(near(L.width(), 330.f), "the rack is as wide as its panels together");
    check(near(L.height(), 380.f), "and exactly one 3U row tall");

    check(!mods[2].known && near(mods[2].width, 120.f),
          "an unknown module gets a bounded fallback width, not zero");

    // A rack is wide; a pane is not. Fit, never magnify.
    check(near(L.fit_scale(1200.f, 460.f), 1.f),
          "a rack that already fits is not magnified");
    check(near(L.fit_scale(165.f, 460.f), 0.5f),
          "a rack wider than the pane is scaled to fit");
    check(near(L.fit_scale(1200.f, 190.f), 0.5f),
          "and height constrains it too");

    // The finding that would silently produce wrong cables.
    const auto in0 = L.endpoint(1, 0, true);
    const auto in1 = L.endpoint(1, 1, true);
    check(in0.exact && near(in0.x, 0.3852f * 135.f),
          "input 0 lands on its recorded jack");
    check(in1.exact && in1.x < in0.x,
          "input 1 is drawn to the LEFT of input 0 — index order is not "
          "visual order");
    check(in0.port_name == "1V/octave pitch",
          "an exact endpoint carries the vendor's own port name");

    const auto out = L.endpoint(1, 2, false);
    check(out.exact && near(out.y, 334.f),
          "an output resolves separately from an input of the same index");

    // The degradation that must never pretend.
    const auto un = L.endpoint(2, 0, true);
    check(!un.exact, "an unmapped module's cable is NOT claimed as exact");
    check(un.y > 300.f, "it docks near the panel's bottom edge instead");
    check(un.port_name.empty(), "and carries no port name it cannot know");

    const auto un_a = L.endpoint(2, 0, true);
    const auto un_b = L.endpoint(2, 1, true);
    check(!near(un_a.x, un_b.x),
          "two cables into one unmapped module stay distinguishable");

    check(L.endpoint(999, 0, true).x == 0.f && !L.endpoint(999, 0, true).exact,
          "a cable to a module that is not in the patch resolves to nothing");

    check(L.has_image("Fundamental/VCO"), "an image is reported when present");
    check(!L.has_image("Fundamental/VCA"), "and not when absent");
    check(!L.has_image("Nobody/Whatever"), "nor for a module we do not know");

    const auto f = L.fidelity();
    check(f.modules == 3 && f.unknown == 1 && f.with_image == 1 && f.mapped == 1,
          "fidelity counts what can actually be drawn faithfully");
    check(!f.exact(),
          "and this patch is honestly reported as not fully exact");

    std::printf("\n%s: %d problem(s)\n", failures ? "FAIL" : "ok", failures);
    return failures ? 1 : 0;
}
