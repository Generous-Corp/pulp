#include "support/waveset_transformer_test_support.hpp"

using namespace pulp::test::waveset;

TEST_CASE("waveset startup CAS linearizes a racing request at the next boundary",
          "[signal][waveset][thread]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Pass)));
    REQUIRE(transformer.set_program(1, program(Transformer::Operation::Reverse)));
    startup_hook_transformer = &transformer;
    Transformer::set_startup_hook(request_reverse_after_startup_cas);
    REQUIRE(drain(transformer, {-2.0f, -1.0f, 1.0f, 2.0f}) ==
            std::vector<float>{-2.0f, -1.0f, 2.0f, 1.0f});
    startup_hook_transformer = nullptr;
}

TEST_CASE("waveset admitted push completes a racing finish request", "[signal][waveset][thread]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
    const std::array<float, 2> input{1.0f, 2.0f};
    startup_hook_transformer = &transformer;
    startup_finish_latched = startup_pull_rejected = false;
    Transformer::set_startup_hook(finish_after_startup_cas);
    REQUIRE(transformer.push(input.data(), 2) == 2);
    REQUIRE(startup_finish_latched);
    REQUIRE(startup_pull_rejected);
    std::array<float, 2> output{};
    REQUIRE(transformer.pull(output.data(), 2) == 2);
    REQUIRE(output == input);
    REQUIRE(transformer.drained());
    startup_hook_transformer = nullptr;
}

TEST_CASE("waveset publication excludes startup and failed replacement preserves old bank",
          "[signal][waveset][thread]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Reverse)));
    publication_hook_transformer = &transformer;
    publication_push_rejected = publication_request_rejected = false;
    Transformer::set_publication_hook(probe_publishing_exclusion);
    REQUIRE(transformer.set_program(1, program(Transformer::Operation::Pass)));
    REQUIRE(publication_push_rejected);
    REQUIRE(publication_request_rejected);
    auto malformed = program(Transformer::Operation::Repeat, 0);
    REQUIRE_FALSE(transformer.set_program(0, malformed));
    REQUIRE(drain(transformer, {1.0f, 2.0f}) == std::vector<float>{2.0f, 1.0f});
    publication_hook_transformer = nullptr;
}

TEST_CASE("waveset finish and publication have one terminal linearization order",
          "[signal][waveset][thread]") {
    const float sample = 1.0f;
    auto require_terminal = [&](Transformer& transformer) {
        REQUIRE(transformer.drained());
        REQUIRE(transformer.push(&sample, 1) == 0);
        REQUIRE_FALSE(transformer.request_program_slot(0));
        REQUIRE_FALSE(transformer.set_program(0, program(Transformer::Operation::Pass)));
    };

    SECTION("finish before publication rejects without bank access") {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
        transformer.finish_input();
        require_terminal(transformer);
    }
    SECTION("finish during successful publication is completed by the publisher") {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
        publication_hook_transformer = &transformer;
        publication_finish_latched = false;
        Transformer::set_publication_hook(finish_during_publication);
        REQUIRE(transformer.set_program(1, program(Transformer::Operation::Reverse)));
        REQUIRE(publication_finish_latched);
        require_terminal(transformer);
        publication_hook_transformer = nullptr;
    }
    SECTION("finish during failed publication preserves the old bank and EOS") {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
        REQUIRE(transformer.set_program(0, program(Transformer::Operation::Reverse)));
        auto malformed = program(Transformer::Operation::Repeat, 0);
        publication_hook_transformer = &transformer;
        publication_finish_latched = false;
        Transformer::set_publication_hook(finish_during_publication);
        REQUIRE_FALSE(transformer.set_program(0, malformed));
        REQUIRE(publication_finish_latched);
        require_terminal(transformer);
        transformer.reset();
        REQUIRE(drain(transformer, {1.0f, 2.0f}) == std::vector<float>{2.0f, 1.0f});
        publication_hook_transformer = nullptr;
    }
    SECTION("finish after publication is terminal") {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
        REQUIRE(transformer.set_program(1, program(Transformer::Operation::Reverse)));
        transformer.finish_input();
        require_terminal(transformer);
    }
}

TEST_CASE("waveset reset joins the unified lifecycle order", "[signal][waveset][thread]") {
    SECTION("publication owns the bank until its terminal store") {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
        publication_hook_transformer = &transformer;
        publication_reset_excluded = false;
        Transformer::set_publication_hook(reset_during_publication);
        REQUIRE(transformer.set_program(1, program(Transformer::Operation::Reverse)));
        REQUIRE(publication_reset_excluded);
        REQUIRE(transformer.request_program_slot(1));
        REQUIRE(drain(transformer, {1.0f, 2.0f}) == std::vector<float>{2.0f, 1.0f});
    }
    SECTION("finishing owns processor state until terminal") {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
        const std::array<float, 2> input{1.0f, 2.0f};
        REQUIRE(transformer.push(input.data(), 2) == 2);
        publication_hook_transformer = &transformer;
        finish_reset_excluded = finish_pull_excluded = false;
        Transformer::set_finish_hook(reset_during_finish);
        transformer.finish_input();
        REQUIRE(finish_reset_excluded);
        REQUIRE(finish_pull_excluded);
        std::array<float, 2> output{};
        REQUIRE(transformer.pull(output.data(), 2) == 2);
        REQUIRE(output == input);
        REQUIRE(transformer.drained());
    }
    SECTION("reset owns processor state until quiescent") {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
        publication_hook_transformer = &transformer;
        reset_finish_excluded = reset_pull_excluded = reset_completion_push_rejected = false;
        Transformer::set_reset_hook(finish_during_reset);
        Transformer::set_reset_completion_hook(push_during_reset_completion);
        transformer.reset();
        REQUIRE(reset_finish_excluded);
        REQUIRE(reset_pull_excluded);
        REQUIRE(reset_completion_push_rejected);
        REQUIRE_FALSE(transformer.drained());
        REQUIRE(drain(transformer, {3.0f, 4.0f}) == std::vector<float>{3.0f, 4.0f});
    }
    publication_hook_transformer = nullptr;
}

TEST_CASE("waveset setter completion transfers latched EOS to processor finalization",
          "[signal][waveset][thread]") {
    SECTION("buffered capture becomes readable terminal output") {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {4, 4, 1.0}));
        const std::array<float, 2> input{1.0f, 2.0f};
        REQUIRE(transformer.push(input.data(), 2) == 2);
        publication_hook_transformer = &transformer;
        control_finish_latched = false;
        Transformer::set_control_completion_hook(finish_during_control_completion);
        REQUIRE(transformer.set_coordinate_seed(7));
        REQUIRE(control_finish_latched);
        std::array<float, 2> output{};
        REQUIRE(transformer.pull(output.data(), 2) == 2);
        REQUIRE(output == input);
        REQUIRE(transformer.drained());
    }
    SECTION("partial Rotate group is flushed before terminal drain") {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
        Transformer::OperationProgram rotate;
        rotate.steps.resize(2);
        for (auto& step : rotate.steps)
            step.operation = Transformer::Operation::Rotate;
        rotate.rotate_window = 2;
        rotate.permutation = {0, 1};
        REQUIRE(transformer.set_program(0, rotate));
        const std::array<float, 2> input{1.0f, 2.0f};
        REQUIRE(transformer.push(input.data(), 2) == 2);
        publication_hook_transformer = &transformer;
        control_finish_latched = false;
        Transformer::set_control_completion_hook(finish_during_control_completion);
        REQUIRE(transformer.set_coordinate_seed(9));
        REQUIRE(control_finish_latched);
        std::array<float, 2> output{};
        REQUIRE(transformer.pull(output.data(), 2) == 2);
        REQUIRE(output == input);
        REQUIRE(transformer.drained());
    }
    publication_hook_transformer = nullptr;
}

TEST_CASE("waveset drained snapshot serializes Finished pull and reset races",
          "[signal][waveset][thread]") {
    SECTION("pull waits until a non-drained snapshot releases ownership") {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
        const float input = 3.0f;
        REQUIRE(transformer.push(&input, 1) == 1);
        transformer.finish_input();
        publication_hook_transformer = &transformer;
        drained_pull_excluded = false;
        Transformer::set_drained_snapshot_hook(pull_during_drained_snapshot);
        REQUIRE_FALSE(transformer.drained());
        REQUIRE(drained_pull_excluded);
        float output{};
        REQUIRE(transformer.pull(&output, 1) == 1);
        REQUIRE(output == input);
        REQUIRE(transformer.drained());
    }
    SECTION("reset waits until a drained snapshot releases ownership") {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
        transformer.finish_input();
        publication_hook_transformer = &transformer;
        drained_reset_excluded = false;
        Transformer::set_drained_snapshot_hook(reset_during_drained_snapshot);
        REQUIRE(transformer.drained());
        REQUIRE(drained_reset_excluded);
        transformer.reset();
        REQUIRE_FALSE(transformer.drained());
        REQUIRE(drain(transformer, {4.0f}) == std::vector<float>{4.0f});
    }
    publication_hook_transformer = nullptr;
}
