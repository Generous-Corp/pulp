// resolved_face_identity() answers with the face a stack will draw. Captured
// line breaks are reused only when it matches the capture's face, so the lite
// editor stack (CoreText) and the full stack (Skia) must give the same answer
// for the faces Pulp bundles. This file builds twice: once against the
// library (full) and once with the lite replacement objects linked ahead of
// it, and both binaries assert the same table.

#include <catch2/catch_test_macros.hpp>

#include <pulp/canvas/font_resolver.hpp>

#include <string>

using pulp::canvas::FontSlant;
using pulp::canvas::resolved_face_identity;

#if PULP_FACE_IDENTITY_LITE
#define PULP_FACE_IDENTITY_STACK "lite"
#else
#define PULP_FACE_IDENTITY_STACK "full"
#endif

TEST_CASE("bundled faces resolve to the expected identity in the " PULP_FACE_IDENTITY_STACK
          " stack",
          "[canvas][font-resolver][lite-editor]") {
    struct Row {
        const char* family;
        float weight;
        const char* face;
    };
    const Row rows[] = {
        {"Inter", 400.0f, "Inter-Regular"},
        {"\"JetBrains Mono\", monospace", 400.0f, "JetBrainsMono-Regular"},
        {"Jost", 400.0f, "Jost-Regular"},
        {"Jost", 500.0f, "Jost-Medium"},
        {"Jost", 600.0f, "Jost-SemiBold"},
        {"Jost", 700.0f, "Jost-Bold"},
    };
    for (const auto& row : rows) {
        INFO(row.family << " " << row.weight);
        CHECK(resolved_face_identity(row.family, row.weight, FontSlant::Normal) == row.face);
    }
}
