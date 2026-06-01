// iOS-D.3c (#3217) regression coverage for the WebGPU mock GPUBuffer's
// mapped-write contract. These run under the DEFAULT JS engine (no V8, no GPU,
// no three.iife bundle) so they execute in every CI configuration — unlike the
// heavier [threejs][gpu][phase13] smoke in test_threejs_bridge.cpp, which needs
// V8 + a real Dawn device and was never even wired into a build target.
//
// The mock prelude (`__createMockGPUDevice`, `__createMockGPUBuffer`, …) is
// installed by the WidgetBridge constructor regardless of engine or GPU, so a
// plain bridge with a null GpuSurface is enough to exercise it.

#include <catch2/catch_test_macros.hpp>

#include <pulp/state/store.hpp>
#include <pulp/view/script_engine.hpp>
#include <pulp/view/theme.hpp>
#include <pulp/view/view.hpp>
#include <pulp/view/widget_bridge.hpp>

#include <string>

using namespace pulp::view;
using namespace pulp;

namespace {

struct MockGpuEnv {
    View root;
    state::StateStore store;
    ScriptEngine engine;
    std::unique_ptr<WidgetBridge> bridge;
    MockGpuEnv() {
        root.set_bounds({0, 0, 64, 64});
        root.set_theme(Theme::dark());
        bridge = std::make_unique<WidgetBridge>(engine, root, store);
        bridge->load_script("");  // install the prelude (mock GPU shims)
    }
    std::string eval(const std::string& code) {
        return std::string(engine.evaluate(code).getWithDefault<std::string_view>(""));
    }
};

}  // namespace

// The mock GPUBuffer used to return `_bytes.buffer.slice(...)` (an independent
// COPY) from getMappedRange() with a no-op unmap(), so writes into a mapped
// range were silently dropped. Three.js's WebGPUBackend uploads geometry with
//   createBuffer({mappedAtCreation:true})
//   new Float32Array(buf.getMappedRange()).set(attr.array)
//   buf.unmap()
// so vertex/index buffers arrived all-zero at the native bridge and meshes
// collapsed to a degenerate point — the render-pass clear still showed, which
// presented as "clear color visible, geometry missing". This pins the
// round-trip: writes into a mapped range MUST land in `_bytes` after unmap().
TEST_CASE("WebGPU mock GPUBuffer: getMappedRange writes commit on unmap (mappedAtCreation upload)",
          "[webcompat][gpu][mock][issue-3217]") {
    MockGpuEnv env;

    // Full-buffer mappedAtCreation upload — exactly Three.js's geometry path.
    // Small integers are exactly representable in float32, so the round-trip
    // stringifies cleanly. Before the fix this returned "0,0,0,0,0,0".
    const auto full = env.eval(R"JS(
        (() => {
            const dev = __createMockGPUDevice(__createMockGPUAdapter({}), {}, {});
            const buf = dev.createBuffer({ size: 24, usage: 0, mappedAtCreation: true });
            new Float32Array(buf.getMappedRange()).set([1, 2, 3, 4, 5, 6]);
            buf.unmap();
            const v = new Float32Array(buf._bytes.buffer, buf._bytes.byteOffset, 6);
            return Array.from(v).join(',');
        })()
    )JS");
    INFO("mappedAtCreation full-range upload => " << full);
    REQUIRE(full == "1,2,3,4,5,6");  // bug: all zeros (writes dropped)

    // Partial range with a byte offset — only the mapped window commits; the
    // rest of the buffer keeps its prior (zero-initialized) contents.
    const auto partial = env.eval(R"JS(
        (() => {
            const dev = __createMockGPUDevice(__createMockGPUAdapter({}), {}, {});
            const buf = dev.createBuffer({ size: 16, usage: 0, mappedAtCreation: true });
            new Float32Array(buf.getMappedRange(8, 8)).set([7, 8]); // bytes 8..15 = floats[2..3]
            buf.unmap();
            const v = new Float32Array(buf._bytes.buffer, buf._bytes.byteOffset, 4);
            return Array.from(v).join(',');
        })()
    )JS");
    INFO("mappedAtCreation partial-range upload => " << partial);
    REQUIRE(partial == "0,0,7,8");  // first two floats untouched, last two written
}

// Companion #3217 coverage (writeBuffer element-vs-byte fix, originally landed
// in PR #3273 but only in the never-built test_threejs_bridge.cpp): writeBuffer's
// dataOffset/size are measured in ELEMENTS when `data` is a TypedArray, not
// bytes. Pinned here so it runs in CI.
TEST_CASE("WebGPU mock GPUQueue.writeBuffer: TypedArray dataOffset/size are elements, not bytes",
          "[webcompat][gpu][mock][issue-3217]") {
    MockGpuEnv env;

    const auto full = env.eval(R"JS(
        (() => {
            const dev = __createMockGPUDevice(__createMockGPUAdapter({}), {}, {});
            const buf = dev.createBuffer({ size: 16, usage: 0 });
            const f = new Float32Array([1, 2, 3, 4]);
            dev.queue.writeBuffer(buf, 0, f, 0, f.length);
            const v = new Float32Array(buf._bytes.buffer, buf._bytes.byteOffset, 4);
            return Array.from(v).join(',');
        })()
    )JS");
    INFO("writeBuffer full-array (element size) => " << full);
    REQUIRE(full == "1,2,3,4");  // bug truncates to "1,0,0,0"

    const auto sliced = env.eval(R"JS(
        (() => {
            const dev = __createMockGPUDevice(__createMockGPUAdapter({}), {}, {});
            const buf = dev.createBuffer({ size: 8, usage: 0 });
            const f = new Float32Array([10, 20, 30, 40]);
            dev.queue.writeBuffer(buf, 0, f, 2, 2); // dataOffset=2 elems, size=2 elems
            const v = new Float32Array(buf._bytes.buffer, buf._bytes.byteOffset, 2);
            return Array.from(v).join(',');
        })()
    )JS");
    INFO("writeBuffer element dataOffset/size => " << sliced);
    REQUIRE(sliced == "30,40");  // bug reads bytes 2..4 -> garbage
}
