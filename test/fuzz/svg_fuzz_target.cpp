// libFuzzer entry point over SVG ingest into the retained vector scene.
//
// Icons and imported artwork arrive as SVG text from design packs and content
// packs, so the document is untrusted input to nanosvg and to the scene
// lowering above it. The parser's contract on a bad document is an empty
// scene, never a crash; the root's bounds are read so the lowered nodes are
// walked, not only constructed.

#include <pulp/canvas/scene/scene.hpp>

#include <cstddef>
#include <cstdint>
#include <string>

extern "C" int LLVMFuzzerTestOneInput(const uint8_t* data, size_t size) {
    const std::string svg(reinterpret_cast<const char*>(data), size);
    auto scene = pulp::canvas::VectorScene::from_svg_string(svg);
    (void)scene.root().child_count();
    (void)scene.root().transformed_local_bounds();
    (void)scene.take_dirty_rect();
    return 0;
}
