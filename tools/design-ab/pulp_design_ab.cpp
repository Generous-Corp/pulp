// Score a rendered panel against the design it was meant to realize.
//
//   pulp-design-ab <reference.png> <ours.png> [--fail-below 0.85]
//
// The harness exists because "does this look right" is not reviewable. Two
// images and a number are. Every fidelity claim in the designed-panel work is
// supposed to be backed by this rather than by an opinion, including mine —
// several times today a render looked fixed and was not, or looked broken and
// was the wrong build.

#include <pulp/view/screenshot_compare.hpp>

#include <cstdio>
#include <cstdlib>
#include <string>

int main(int argc, char** argv) {
    if (argc < 3) {
        std::fprintf(stderr,
                     "usage: pulp-design-ab <reference.png> <ours.png> "
                     "[--fail-below <0..1>]\n");
        return 2;
    }
    const std::string reference = argv[1];
    const std::string ours = argv[2];
    float gate = -1.0f;
    for (int i = 3; i + 1 < argc; ++i) {
        if (std::string(argv[i]) == "--fail-below") gate = std::atof(argv[i + 1]);
    }

    const auto result = pulp::view::compare_screenshot_files(reference, ours);
    if (!result.valid) {
        std::fprintf(stderr, "comparison failed: %s\n", result.error.c_str());
        return 2;
    }

    std::printf("similarity %.1f%%  (%u/%u pixels differ, mean error %.1f)\n",
                result.similarity * 100.0f, result.diff_pixels,
                result.total_pixels, result.mean_error);

    // A gate is only meaningful when the caller states it, so an unattended run
    // reports and a deliberate one enforces.
    if (gate >= 0.0f) {
        const bool pass = result.similarity >= gate;
        std::printf("%s (gate %.0f%%)\n", pass ? "PASS" : "NEEDS REVIEW",
                    gate * 100.0f);
        return pass ? 0 : 1;
    }
    return 0;
}
