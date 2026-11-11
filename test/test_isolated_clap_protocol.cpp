// SPDX-License-Identifier: MIT
#include "isolated_clap_endpoint.hpp"
#include <array>
#include <catch2/catch_test_macros.hpp>
#include <limits>
using namespace pulp;
using namespace pulp::host;
TEST_CASE("Isolated CLAP callback validates adversarial peer publications", "[host][isolation]") {
    using namespace host::isolation;
    for (int defect = 0; defect < 9; ++defect) {
        Mapping mapping{};
        CallbackEndpoint endpoint;
        std::array<float, 64> in{}, left{}, right{};
        in.fill(0.25f);
        REQUIRE(endpoint.render(mapping, 17, left.data(), right.data(), in.data(), in.data()) ==
                IsolationReason::none);
        auto& completion = mapping.completions[0];
        completion.block.generation = 17;
        completion.block.sequence = 0;
        completion.block.audio.fill(0.125f);
        completion.ready.store(1, std::memory_order_release);
        if (defect == 0)
            completion.block.generation = 16;
        if (defect == 1)
            completion.block.sequence = 1;
        if (defect == 2)
            completion.block.frames = 63;
        if (defect == 3)
            completion.block.channels = 1;
        if (defect == 4)
            completion.block.result = static_cast<BlockResult>(99);
        if (defect == 5)
            completion.block.audio[127] = std::numeric_limits<float>::quiet_NaN();
        if (defect == 6)
            mapping.requests[1].ready.store(1, std::memory_order_release);
        if (defect == 7)
            completion.ready.store(0, std::memory_order_release);
        const auto reason =
            endpoint.render(mapping, 17, left.data(), right.data(), in.data(), in.data());
        if (defect == 8) {
            REQUIRE(reason == IsolationReason::none);
            REQUIRE(left[0] == 0.125f);
            REQUIRE(right[63] == 0.125f);
        } else {
            REQUIRE(reason != IsolationReason::none);
            for (float sample : left)
                REQUIRE(sample == 0);
            for (float sample : right)
                REQUIRE(sample == 0);
        }
    }
}
