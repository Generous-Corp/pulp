#include <array>
#include <atomic>
#include <catch2/catch_test_macros.hpp>
#include <cstring>
#include <future>
#include <pulp/host/signal_graph.hpp>
#include <pulp/host/signal_graph_prepared_topology_edit.hpp>

using namespace pulp::host;
namespace {
using Availability = CustomNodeDiagnosticAvailability;
struct State {
    std::uint64_t generation = 0;
    std::atomic<std::uint64_t> delivered{0};
    bool prepared = false;
};
struct Report {
    std::uint64_t delivered = 0;
};
CustomNodeType type() {
    CustomNodeType t;
    t.type_id = "diagnostic.test";
    t.version = 1;
    t.num_input_ports = 1;
    t.num_output_ports = 1;
    t.create = []() -> void* { return new State{}; };
    t.destroy = [](void* p) { delete static_cast<State*>(p); };
    t.prepare = [](void* p, double, int) {
        auto& s = *static_cast<State*>(p);
        ++s.generation;
        s.prepared = true;
    };
    t.release = [](void* p) { static_cast<State*>(p)->prepared = false; };
    t.process_instance = [](void* p, auto& output, const auto&, int) {
        ++static_cast<State*>(p)->delivered;
        output.clear();
    };
    return t;
}
Availability query_state(const void* p, std::span<std::byte> bytes,
                         std::uint64_t& generation) noexcept {
    const auto& s = *static_cast<const State*>(p);
    if (!s.prepared)
        return Availability::NotPrepared;
    const Report report{s.delivered.load(std::memory_order_relaxed)};
    std::memcpy(bytes.data(), &report, sizeof(report));
    generation = s.generation;
    return Availability::Available;
}
CustomNodeDiagnosticsDescriptor descriptor() {
    return {"diagnostic.test", 1, 123, sizeof(Report), &query_state};
}
NodeId setup(SignalGraph& g) {
    REQUIRE(g.register_custom_node_type(type()));
    REQUIRE(g.register_custom_node_diagnostics(descriptor()));
    const auto id = g.add_custom_node("diagnostic.test");
    const auto output = g.add_output_node(1);
    REQUIRE(g.connect(id, 0, output, 0));
    return id;
}
} // namespace

TEST_CASE("custom diagnostics require exact graph generation schema and prepared instance",
          "[host][signal-graph][diagnostics]") {
    SignalGraph g;
    const auto id = setup(g);
    Report report{99};
    auto handle = g.custom_node_diagnostic_handle(id);
    REQUIRE(handle.availability == Availability::Available);
    CHECK(g.query_custom_node_diagnostics(handle.handle, 123, report).availability ==
          Availability::NotPrepared);
    CHECK(report.delivered == 0);
    REQUIRE(g.prepare(48000, 32));
    CHECK(g.query_custom_node_diagnostics(handle.handle, 123, report).availability ==
          Availability::StaleHandle);
    handle = g.custom_node_diagnostic_handle(id);
    CHECK(std::string(handle.type_id.data()) == "diagnostic.test");
    CHECK(std::string(handle.producer_type_id.data()) == "diagnostic.test");
    CHECK(g.query_custom_node_diagnostics(handle.handle, 456, report).availability ==
          Availability::SchemaMismatch);
    std::array<std::byte, sizeof(Report) + 1> unaligned{};
    const auto bytes = std::span<std::byte>(unaligned).subspan(1);
    const auto result = g.query_custom_node_diagnostics(
        handle.handle, 123, bytes, CustomNodeDiagnosticConsistency::AudioCallerStopped);
    REQUIRE(result.availability == Availability::Available);
    CHECK(result.preparation_generation == 1);
    std::array<float, 32> samples{};
    float* out_ptrs[] = {samples.data()};
    const float* in_ptrs[] = {samples.data()};
    pulp::audio::BufferView<float> out(out_ptrs, 1, 32);
    pulp::audio::BufferView<const float> in(in_ptrs, 1, 32);
    g.process(out, in, 32);
    CHECK(g.query_custom_node_diagnostics(handle.handle, 123, report).availability ==
          Availability::Available);
    CHECK(report.delivered == 1);
    CHECK(result.consistency == CustomNodeDiagnosticConsistency::AudioCallerStopped);
    auto missing = handle.handle;
    missing.node_id = 0xffffffffu;
    CHECK(g.query_custom_node_diagnostics(missing, 123, report).availability ==
          Availability::MissingNode);
    std::array<std::byte, 4097> oversized{};
    oversized[0] = std::byte{42};
    CHECK(g.query_custom_node_diagnostics(handle.handle, 123, std::span<std::byte>(oversized))
              .availability == Availability::SchemaMismatch);
    CHECK(oversized[0] == std::byte{42});
    std::array<std::byte, 1> too_small{};
    CHECK(g.query_custom_node_diagnostics(handle.handle, 123, std::span<std::byte>(too_small))
              .availability == Availability::SchemaMismatch);
    SignalGraph other;
    CHECK(other.query_custom_node_diagnostics(handle.handle, 123, report).availability ==
          Availability::StaleHandle);
    REQUIRE(g.prepare(48000, 64));
    CHECK(g.query_custom_node_diagnostics(handle.handle, 123, report).availability ==
          Availability::StaleHandle);
    handle = g.custom_node_diagnostic_handle(id);
    CHECK(g.query_custom_node_diagnostics(handle.handle, 123, report).preparation_generation == 2);
    g.release();
    CHECK(g.query_custom_node_diagnostics(handle.handle, 123, report).availability ==
          Availability::StaleHandle);
    handle = g.custom_node_diagnostic_handle(id);
    CHECK(g.query_custom_node_diagnostics(handle.handle, 123, report).availability ==
          Availability::NotPrepared);
    g.remove_node(id);
    CHECK(g.custom_node_diagnostic_handle(id).availability == Availability::MissingNode);
}

TEST_CASE("custom diagnostic companions fail closed for missing type and replacement",
          "[host][signal-graph][diagnostics]") {
    SignalGraph g;
    CHECK_FALSE(g.register_custom_node_diagnostics(descriptor()));
    REQUIRE(g.register_custom_node_type(type()));
    auto invalid = descriptor();
    invalid.report_bytes = 4097;
    CHECK_FALSE(g.register_custom_node_diagnostics(invalid));
    invalid = descriptor();
    invalid.type_version = 2;
    CHECK_FALSE(g.register_custom_node_diagnostics(invalid));
    const auto id = g.add_custom_node("diagnostic.test");
    CHECK(g.custom_node_diagnostic_handle(id).availability == Availability::Unsupported);
    REQUIRE(g.register_custom_node_diagnostics(descriptor()));
    REQUIRE(g.register_custom_node_type(type()));
    CHECK(g.custom_node_diagnostic_handle(id).availability == Availability::Unsupported);
    CHECK(g.custom_node_diagnostic_handle(g.add_gain_node()).availability ==
          Availability::Unsupported);
}

TEST_CASE("custom diagnostics return busy while release owns the instance lifecycle",
          "[host][signal-graph][diagnostics]") {
    SignalGraph g;
    std::promise<void> entered, resume;
    auto resume_future = resume.get_future().share();
    auto t = type();
    t.release = [&](void* p) {
        entered.set_value();
        resume_future.wait();
        static_cast<State*>(p)->prepared = false;
    };
    REQUIRE(g.register_custom_node_type(std::move(t)));
    REQUIRE(g.register_custom_node_diagnostics(descriptor()));
    const auto id = g.add_custom_node("diagnostic.test");
    REQUIRE(g.prepare(48000, 32));
    const auto handle = g.custom_node_diagnostic_handle(id).handle;
    auto released = std::async(std::launch::async, [&] { g.release(); });
    const auto ready = entered.get_future().wait_for(std::chrono::seconds(5));
    Report report;
    const auto result = ready == std::future_status::ready
                            ? g.query_custom_node_diagnostics(handle, 123, report).availability
                            : Availability::Unsupported;
    const auto handle_availability = ready == std::future_status::ready
                                         ? g.custom_node_diagnostic_handle(id).availability
                                         : Availability::Unsupported;
    resume.set_value();
    released.get();
    REQUIRE(ready == std::future_status::ready);
    CHECK(result == Availability::Busy);
    CHECK(handle_availability == Availability::Busy);
}

TEST_CASE("prepared topology keeps matching diagnostic companion and invalidates old handle",
          "[host][signal-graph][diagnostics]") {
    SignalGraph g;
    const auto id = setup(g);
    REQUIRE(g.prepare(48000, 32));
    const auto old = g.custom_node_diagnostic_handle(id).handle;
    auto edit = g.begin_prepared_topology_edit();
    edit->add_gain_node("additional node");
    REQUIRE(edit->prepare(48000, 32) == SignalGraph::PreparedTopologyEdit::Result::Prepared);
    REQUIRE(edit->commit() == SignalGraph::PreparedTopologyEdit::Result::Committed);
    Report report;
    CHECK(g.query_custom_node_diagnostics(old, 123, report).availability ==
          Availability::StaleHandle);
    const auto current = g.custom_node_diagnostic_handle(id).handle;
    const auto result = g.query_custom_node_diagnostics(current, 123, report);
    CHECK(result.availability == Availability::Available);
    CHECK(result.preparation_generation == 1); // Retained instances were not reprepared.
    g.release();
}

TEST_CASE("diagnostic alias retains original producer identity without exposing its instance",
          "[host][signal-graph][diagnostics]") {
    SignalGraph g;
    struct Wrapper {
        std::uint64_t sentinel = 999;
        State inner;
    };
    auto t = type();
    t.type_id = "diagnostic.alias";
    t.create = []() -> void* { return new Wrapper{}; };
    t.destroy = [](void* p) { delete static_cast<Wrapper*>(p); };
    t.prepare = [](void* p, double, int) {
        auto& inner = static_cast<Wrapper*>(p)->inner;
        inner.prepared = true;
        inner.generation = 7;
        inner.delivered = 42;
    };
    t.release = [](void* p) { static_cast<Wrapper*>(p)->inner.prepared = false; };
    REQUIRE(g.register_custom_node_type(t));
    auto d = descriptor();
    d.type_id = t.type_id;
    d.producer_type_id = "diagnostic.test";
    d.producer_type_version = 1;
    d.query = [](const void* p, std::span<std::byte> bytes, std::uint64_t& generation) noexcept {
        return query_state(&static_cast<const Wrapper*>(p)->inner, bytes, generation);
    };
    REQUIRE(g.register_custom_node_diagnostics(d));
    const auto id = g.add_custom_node(t.type_id);
    REQUIRE(g.prepare(48000, 32));
    const auto handle = g.custom_node_diagnostic_handle(id);
    Report report;
    const auto result = g.query_custom_node_diagnostics(handle.handle, 123, report);
    CHECK(result.availability == Availability::Available);
    CHECK(std::string(result.type_id.data()) == "diagnostic.alias");
    CHECK(std::string(result.producer_type_id.data()) == "diagnostic.test");
    CHECK(result.producer_type_version == 1);
    CHECK(result.preparation_generation == 7);
    CHECK(report.delivered == 42);
    g.release();
}
