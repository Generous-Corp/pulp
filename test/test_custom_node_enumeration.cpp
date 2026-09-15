#include <catch2/catch_test_macros.hpp>
#include <pulp/host/signal_graph.hpp>

#include <algorithm>
#include <atomic>
#include <string>
#include <thread>
#include <utility>
#include <vector>

using pulp::host::CustomNodeBakedParam;
using pulp::host::CustomNodeType;
using pulp::host::CustomNodeTypeMetadata;
using pulp::host::SignalGraph;

namespace {

CustomNodeType make_type(std::string id, int version, std::string name) {
    CustomNodeType type;
    type.type_id = std::move(id);
    type.version = version;
    type.num_input_ports = version % 3;
    type.num_output_ports = (version + 1) % 3;
    type.default_name = std::move(name);
    type.process = [](auto&, const auto&, int) {};
    return type;
}

bool is_sorted(const std::vector<CustomNodeTypeMetadata>& snapshot) {
    return std::is_sorted(snapshot.begin(), snapshot.end(), [](const auto& lhs, const auto& rhs) {
        if (lhs.type_id != rhs.type_id)
            return lhs.type_id < rhs.type_id;
        return lhs.version < rhs.version;
    });
}

} // namespace

TEST_CASE("custom-node metadata enumeration is deterministic and value-owned",
          "[host][graph][custom-node][enumeration]") {
    std::vector<CustomNodeTypeMetadata> retained;
    {
        SignalGraph graph;
        REQUIRE(graph.custom_node_types().empty());

        auto beta_v2 = make_type("pulp.test.beta", 2, "Beta Two");
        beta_v2.lowerable = true;
        beta_v2.create = [] { return static_cast<void*>(new int{0}); };
        beta_v2.destroy = [](void* instance) { delete static_cast<int*>(instance); };
        beta_v2.process_instance = [](void*, auto&, const auto&, int) {};
        beta_v2.baked_params = {CustomNodeBakedParam{41, -1.0f, 1.0f, 0.25f}};
        beta_v2.process_instance_baked_param = [](void*, auto&, const auto&, int, const auto&) {};
        REQUIRE(graph.register_custom_node_type(std::move(beta_v2)));
        REQUIRE(graph.register_custom_node_type(make_type("pulp.test.alpha", 3, "Alpha Three")));
        REQUIRE(graph.register_custom_node_type(make_type("pulp.test.alpha", 1, "Alpha One")));

        retained = graph.custom_node_types();
        REQUIRE(retained.size() == 3);
        REQUIRE(is_sorted(retained));
        CHECK(retained[0].type_id == "pulp.test.alpha");
        CHECK(retained[0].version == 1);
        CHECK(retained[1].type_id == "pulp.test.alpha");
        CHECK(retained[1].version == 3);
        CHECK(retained[2].type_id == "pulp.test.beta");
        CHECK(retained[2].version == 2);
        CHECK(retained[2].lowerable);
        REQUIRE(retained[2].baked_params.size() == 1);
        CHECK(retained[2].baked_params[0].id == 41);

        // Re-registering an exact key retains the historical replacement
        // behavior and produces one updated metadata row.
        REQUIRE(
            graph.register_custom_node_type(make_type("pulp.test.alpha", 1, "Alpha One Replaced")));
        const auto replaced = graph.custom_node_types();
        REQUIRE(replaced.size() == 3);
        CHECK(replaced[0].default_name == "Alpha One Replaced");

        // Existing exact/latest lookup semantics remain intact.
        REQUIRE(graph.custom_node_type("pulp.test.alpha", 1) != nullptr);
        REQUIRE(graph.custom_node_type("pulp.test.alpha", 2) == nullptr);
        REQUIRE(graph.custom_node_type("pulp.test.alpha") != nullptr);
        CHECK(graph.custom_node_type("pulp.test.alpha")->version == 3);
        CHECK(graph.custom_node_type_count() == 3);

        auto invalid = make_type("", 1, "Invalid");
        CHECK_FALSE(graph.register_custom_node_type(std::move(invalid)));
        CHECK(graph.custom_node_type_count() == 3);
    }

    // Both strings and nested vectors belong to the returned snapshot.
    CHECK(retained[2].type_id == "pulp.test.beta");
    CHECK(retained[2].default_name == "Beta Two");
    REQUIRE(retained[2].baked_params.size() == 1);
    CHECK(retained[2].baked_params[0].default_value == 0.25f);
}

TEST_CASE("custom-node registration and metadata enumeration share a safe snapshot boundary",
          "[host][graph][custom-node][enumeration][concurrency]") {
    SignalGraph graph;
    std::atomic<bool> start{false};
    std::atomic<bool> coherent{true};

    std::thread registrar([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int version = 1; version <= 2048; ++version) {
            if (!graph.register_custom_node_type(make_type(
                    "pulp.test.concurrent", version, "Concurrent " + std::to_string(version)))) {
                coherent.store(false, std::memory_order_relaxed);
                return;
            }
        }
    });
    std::thread enumerator([&] {
        while (!start.load(std::memory_order_acquire))
            std::this_thread::yield();
        for (int pass = 0; pass < 2048; ++pass) {
            const auto snapshot = graph.custom_node_types();
            if (!is_sorted(snapshot)) {
                coherent.store(false, std::memory_order_relaxed);
                return;
            }
            for (const auto& metadata : snapshot) {
                if (metadata.type_id != "pulp.test.concurrent" ||
                    metadata.default_name != "Concurrent " + std::to_string(metadata.version)) {
                    coherent.store(false, std::memory_order_relaxed);
                    return;
                }
            }
        }
    });

    start.store(true, std::memory_order_release);
    registrar.join();
    enumerator.join();

    REQUIRE(coherent.load(std::memory_order_relaxed));
    const auto complete = graph.custom_node_types();
    REQUIRE(complete.size() == 2048);
    REQUIRE(is_sorted(complete));
}
