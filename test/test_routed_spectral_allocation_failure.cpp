#include <cstdlib>
#include <new>

#include <array>
#include <catch2/catch_test_macros.hpp>
#include <cmath>
#include <cstdint>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

namespace allocation_failure {
thread_local bool armed = false;
thread_local std::size_t fail_after = 0;
thread_local std::size_t attempts = 0;
struct Record {
    void* pointer = nullptr;
    bool armed = false;
};
std::array<Record, 1 << 20> records{};
std::size_t record_count = 0;
std::ptrdiff_t window_balance = 0;

void arm(std::size_t n) noexcept {
    fail_after = n;
    attempts = 0;
    armed = true;
}

void arm_count_only() noexcept {
    attempts = 0;
    armed = true;
    fail_after = static_cast<std::size_t>(-1);
}

void disarm() noexcept {
    armed = false;
}

void* allocate(std::size_t bytes) {
    if (armed) {
        const auto ordinal = attempts++;
        if (ordinal == fail_after)
            throw std::bad_alloc();
    }
    const auto actual = bytes == 0 ? 1 : bytes;
    void* result = std::malloc(actual);
    if (result == nullptr)
        throw std::bad_alloc();
    records[record_count++] = {result, armed};
    if (armed)
        ++window_balance;
    return result;
}

void* allocate_nothrow(std::size_t bytes) noexcept {
    try {
        return allocate(bytes);
    } catch (...) {
        return nullptr;
    }
}

void* allocate_aligned(std::size_t bytes, std::align_val_t alignment) {
    if (armed) {
        const auto ordinal = attempts++;
        if (ordinal == fail_after)
            throw std::bad_alloc();
    }
    void* result = nullptr;
    const auto actual = bytes == 0 ? 1 : bytes;
    if (posix_memalign(&result, static_cast<std::size_t>(alignment), actual) != 0)
        throw std::bad_alloc();
    records[record_count++] = {result, armed};
    if (armed)
        ++window_balance;
    return result;
}

void* allocate_aligned_nothrow(std::size_t bytes, std::align_val_t alignment) noexcept {
    try {
        return allocate_aligned(bytes, alignment);
    } catch (...) {
        return nullptr;
    }
}

void release(void* ptr) noexcept {
    for (std::size_t i = 0; i < record_count; ++i) {
        if (records[i].pointer != ptr)
            continue;
        if (records[i].armed)
            --window_balance;
        records[i] = records[--record_count];
        break;
    }
    std::free(ptr);
}
} // namespace allocation_failure

void* operator new(std::size_t bytes) {
    return allocation_failure::allocate(bytes);
}
void* operator new[](std::size_t bytes) {
    return allocation_failure::allocate(bytes);
}
void* operator new(std::size_t bytes, const std::nothrow_t&) noexcept {
    return allocation_failure::allocate_nothrow(bytes);
}
void* operator new[](std::size_t bytes, const std::nothrow_t&) noexcept {
    return allocation_failure::allocate_nothrow(bytes);
}
void operator delete(void* ptr) noexcept {
    allocation_failure::release(ptr);
}
void operator delete[](void* ptr) noexcept {
    allocation_failure::release(ptr);
}
void operator delete(void* ptr, std::size_t) noexcept {
    allocation_failure::release(ptr);
}
void operator delete[](void* ptr, std::size_t) noexcept {
    allocation_failure::release(ptr);
}
void operator delete(void* ptr, const std::nothrow_t&) noexcept {
    allocation_failure::release(ptr);
}
void operator delete[](void* ptr, const std::nothrow_t&) noexcept {
    allocation_failure::release(ptr);
}
void* operator new(std::size_t bytes, std::align_val_t alignment) {
    return allocation_failure::allocate_aligned(bytes, alignment);
}
void* operator new[](std::size_t bytes, std::align_val_t alignment) {
    return allocation_failure::allocate_aligned(bytes, alignment);
}
void* operator new(std::size_t bytes, std::align_val_t alignment, const std::nothrow_t&) noexcept {
    return allocation_failure::allocate_aligned_nothrow(bytes, alignment);
}
void* operator new[](std::size_t bytes, std::align_val_t alignment,
                     const std::nothrow_t&) noexcept {
    return allocation_failure::allocate_aligned_nothrow(bytes, alignment);
}
void operator delete(void* ptr, std::align_val_t) noexcept {
    allocation_failure::release(ptr);
}
void operator delete[](void* ptr, std::align_val_t) noexcept {
    allocation_failure::release(ptr);
}
void operator delete(void* ptr, std::size_t, std::align_val_t) noexcept {
    allocation_failure::release(ptr);
}
void operator delete[](void* ptr, std::size_t, std::align_val_t) noexcept {
    allocation_failure::release(ptr);
}
void operator delete(void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
    allocation_failure::release(ptr);
}
void operator delete[](void* ptr, std::align_val_t, const std::nothrow_t&) noexcept {
    allocation_failure::release(ptr);
}

namespace {
using pulp::host::SignalGraph;

struct Fixture {
    SignalGraph graph;
    pulp::host::NodeId input{};
    pulp::host::NodeId output{};
    pulp::host::NodeId analysis_node{};
    pulp::host::NodeId synthesis_node{};
    pulp::host::SpectralOutputPort analysis_frames;
    pulp::host::SpectralInputPort synthesis_frames;

    Fixture() {
        using namespace pulp::host;
        const auto shape = *SpectralIdentity::make(256, 64, 1, 48000).value;
        input = graph.add_input_node(1);
        output = graph.add_output_node(1);
        const auto analysis = *graph.add_spectral_analysis(shape, "analysis").value;
        const auto synthesis = *graph.add_spectral_synthesis(shape, "synthesis").value;
        analysis_node = analysis.node;
        synthesis_node = synthesis.node;
        analysis_frames = analysis.frames;
        synthesis_frames = synthesis.frames;
        REQUIRE(graph.connect(graph.node(input)->out("audio"), analysis.audio[0]));
        REQUIRE(graph.connect(analysis.frames, synthesis.frames));
        REQUIRE(graph.connect(synthesis.audio[0], graph.node(output)->in("audio")));
        REQUIRE(graph.prepare(48000, 64));
    }
};

std::array<float, 64> render(SignalGraph& graph) {
    std::array<float, 64> input{};
    std::array<float, 64> output{};
    input[0] = 1.0f;
    input[17] = -0.25f;
    const float* inputs[] = {input.data()};
    float* outputs[] = {output.data()};
    pulp::audio::BufferView<const float> in(inputs, 1, input.size());
    pulp::audio::BufferView<float> out(outputs, 1, output.size());
    graph.process(out, in, static_cast<int>(input.size()));
    return output;
}

SignalGraph::PreparedTopologyEdit* make_edit(Fixture& fixture) {
    auto edit = fixture.graph.begin_prepared_topology_edit();
    REQUIRE(edit);
    const auto shape = *pulp::host::SpectralIdentity::make(256, 64, 1, 48000).value;
    const auto stage = edit->add_spectral_stage(
        shape, *pulp::host::QualifiedSpectralStage::identity_v1(shape).value, "identity");
    REQUIRE(stage);
    REQUIRE(edit->disconnect(fixture.analysis_node, 0, fixture.synthesis_node, 0));
    REQUIRE(edit->connect(fixture.analysis_frames, stage.value->frames_in));
    REQUIRE(edit->connect(stage.value->frames_out, fixture.synthesis_frames));
    return edit.release();
}
} // namespace

TEST_CASE("Routed spectral candidate preparation survives every injected allocation failure",
          "[host][spectral][allocation-failure]") {
    Fixture counting;
    auto* counting_edit = make_edit(counting);
    allocation_failure::arm_count_only();
    const auto count_result = counting_edit->prepare(48000, 64);
    const auto allocation_points = allocation_failure::attempts;
    allocation_failure::disarm();
    REQUIRE(count_result == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(allocation_points > 0);
    delete counting_edit;
    Fixture control;
    const auto expected = render(control.graph);
    bool reached_uninjected_completion = false;
    for (std::size_t n = 0; n <= allocation_points; ++n) {
        Fixture fixture;
        auto* edit = make_edit(fixture);
        const auto before = allocation_failure::window_balance;
        allocation_failure::arm(n);
        const auto result = edit->prepare(48000, 64);
        allocation_failure::disarm();
        if (result == SignalGraph::PreparedTopologyEdit::Result::Prepared) {
            reached_uninjected_completion = true;
            delete edit;
            break;
        }
        const bool failure_ok =
            result == SignalGraph::PreparedTopologyEdit::Result::PreflightFailed ||
            result == SignalGraph::PreparedTopologyEdit::Result::CompileFailed;
        CAPTURE(n, allocation_failure::attempts);
        INFO("result=" << static_cast<int>(result));
        REQUIRE(failure_ok);
        REQUIRE(render(fixture.graph) == expected);
        delete edit;
        const bool balance_ok = allocation_failure::window_balance == before;
        REQUIRE(balance_ok);

        auto retry = fixture.graph.begin_prepared_topology_edit();
        REQUIRE(retry);
        REQUIRE(retry->prepare(48000, 64) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    }
    REQUIRE(reached_uninjected_completion);
}
