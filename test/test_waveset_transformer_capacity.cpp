#include "support/waveset_transformer_test_support.hpp"

using namespace pulp::test::waveset;

TEST_CASE("waveset E Q D T reservation produces an exact retryable short write",
          "[signal][waveset][bounds]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {1, 2, 1.0}));
    REQUIRE(transformer.latency_samples() == 0);
    REQUIRE(transformer.max_lookahead_samples() == 4);
    REQUIRE(transformer.tail_samples() == 84);
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Repeat, 16)));
    const std::array<float, 6> input{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f};

    REQUIRE(transformer.push(input.data(), static_cast<int>(input.size())) == 4);
    std::array<float, 128> buffer{};
    std::vector<float> output;
    const int first_pull = transformer.pull(buffer.data(), 32);
    REQUIRE(first_pull == 32);
    output.insert(output.end(), buffer.begin(), buffer.begin() + first_pull);
    REQUIRE(transformer.push(input.data() + 4, 2) == 2);
    transformer.finish_input();
    while (!transformer.drained()) {
        const int pulled = transformer.pull(buffer.data(), static_cast<int>(buffer.size()));
        REQUIRE(pulled > 0);
        output.insert(output.end(), buffer.begin(), buffer.begin() + pulled);
    }
    REQUIRE(output.size() == 96);
    std::vector<float> expected;
    for (std::size_t segment = 0; segment < 3; ++segment)
        for (int copy = 0; copy < 16; ++copy) {
            expected.push_back(input[segment * 2]);
            expected.push_back(input[segment * 2 + 1]);
        }
    REQUIRE(output == expected);
}

TEST_CASE("waveset eager pull and reservation progress cannot capacity-deadlock",
          "[signal][waveset][bounds]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {4, 1000, 1.0}));
    REQUIRE(transformer.latency_samples() == 0);
    REQUIRE(transformer.max_lookahead_samples() == 8000);
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Pass)));
    transformer.set_zero_crossing_polarity(Transformer::ZeroCrossingPolarity::Both);
    std::array<float, 128> alternating{};
    for (std::size_t i = 0; i < alternating.size(); ++i)
        alternating[i] = (i & 1u) ? 1.0f : -1.0f;
    std::array<float, 8> output{};
    std::size_t accepted_total = 0;
    bool pulled_before_latency = false;
    while (accepted_total < alternating.size()) {
        const int pushed = transformer.push(alternating.data() + accepted_total,
                                            static_cast<int>(alternating.size() - accepted_total));
        accepted_total += static_cast<std::size_t>(pushed);
        const int pulled = transformer.pull(output.data(), static_cast<int>(output.size()));
        pulled_before_latency = pulled_before_latency || pulled > 0;
        REQUIRE((pushed > 0 || pulled > 0));
        if (accepted_total < alternating.size() && pushed == 0)
            REQUIRE(pulled > 0);
    }
    REQUIRE(accepted_total < static_cast<std::size_t>(transformer.max_lookahead_samples()));
    REQUIRE(pulled_before_latency);
}

TEST_CASE("waveset Rotate window accepts 256 and rejects 257", "[signal][waveset]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {257, 1, 1.0}));
    Transformer::OperationProgram rotate;
    rotate.steps.resize(256);
    rotate.permutation.resize(256);
    rotate.rotate_window = 256;
    for (std::size_t i = 0; i < 256; ++i) {
        rotate.steps[i].operation = Transformer::Operation::Rotate;
        rotate.permutation[i] = static_cast<std::uint16_t>(i);
    }
    REQUIRE(transformer.set_program(0, rotate));
    rotate.steps.resize(257);
    rotate.permutation.resize(257);
    rotate.steps.back().operation = Transformer::Operation::Rotate;
    rotate.permutation.back() = 256;
    rotate.rotate_window = 257;
    REQUIRE_FALSE(transformer.set_program(1, rotate));
}

TEST_CASE("waveset Q accounts for unread output held Rotate reservation and splice holdback",
          "[signal][waveset][bounds]") {
    Transformer transformer;
    constexpr int n = 3;
    constexpr int m = 40;
    constexpr int e = Transformer::kMaxRepeatCount * m;
    constexpr int f = 20;
    constexpr int q = (n + 1) * e + f;
    REQUIRE(transformer.prepare(1000.0, {n, m, 1.0}));
    REQUIRE(transformer.latency_samples() == 0);
    REQUIRE(transformer.max_lookahead_samples() == 2 * n * m);
    REQUIRE(transformer.tail_samples() == q);

    Transformer::OperationProgram operations;
    operations.steps = {{Transformer::Operation::Repeat, 16},
                        {Transformer::Operation::Rotate},
                        {Transformer::Operation::Rotate}};
    operations.rotate_window = 2;
    operations.permutation = {0, 1};
    REQUIRE(transformer.set_program(0, operations));
    std::vector<float> input(9u * m);
    for (std::size_t segment = 0; segment < 9; ++segment)
        std::fill_n(input.begin() + static_cast<std::ptrdiff_t>(segment * m), m,
                    static_cast<float>(segment + 1u));

    // Five segments create 1,360 unread samples plus one Rotate-held E. Q has
    // only 580 free, less than the next segment's E=640 reservation.
    REQUIRE(transformer.push(input.data(), static_cast<int>(input.size())) == 5 * m);
    std::array<float, 2048> block{};
    // Pulling one reservation's worth leaves the F=20 splice holdback unread.
    REQUIRE(transformer.pull(block.data(), e) == e);
    REQUIRE(transformer.push(input.data() + 5 * m, 4 * m) == 3 * m);
    REQUIRE(transformer.pull(block.data() + e, e) == e);
    REQUIRE(transformer.push(input.data() + 8 * m, m) == m);
    transformer.finish_input();
    std::vector<float> output(block.begin(), block.begin() + 2 * e);
    while (!transformer.drained()) {
        const int pulled = transformer.pull(block.data(), static_cast<int>(block.size()));
        REQUIRE(pulled > 0);
        output.insert(output.end(), block.begin(), block.begin() + pulled);
    }
    std::vector<float> expected;
    for (const int value : {1, 2, 3, 4, 5, 6, 7, 8, 9}) {
        const int copies = (value == 1 || value == 4 || value == 7) ? 16 : 1;
        expected.insert(expected.end(), static_cast<std::size_t>(copies * m),
                        static_cast<float>(value));
    }
    REQUIRE(output == expected);
}

TEST_CASE("waveset edited splices use exact clamped shared gain laws", "[signal][waveset]") {
    Transformer equal_gain;
    REQUIRE(equal_gain.prepare(1000.0, {2, 8, 1.0}));
    REQUIRE(equal_gain.set_program(0, program(Transformer::Operation::Repeat, 2)));
    equal_gain.set_crossfade_law(pulp::signal::CrossfadeGainLaw::EqualGain);
    equal_gain.set_crossfade_duration_ms(2.0f);
    REQUIRE(drain(equal_gain, {1.0f, 2.0f, 3.0f, 4.0f}) ==
            std::vector<float>{1.0f, 2.0f, 3.0f, 2.0f, 3.0f, 4.0f});

    Transformer equal_power;
    REQUIRE(equal_power.prepare(1000.0, {2, 8, 1.0}));
    REQUIRE(equal_power.set_program(0, program(Transformer::Operation::Repeat, 2)));
    equal_power.set_crossfade_law(pulp::signal::CrossfadeGainLaw::EqualPower);
    equal_power.set_crossfade_duration_ms(3.0f);
    const auto correlated = drain(equal_power, std::vector<float>(6, 1.0f));
    REQUIRE(correlated.size() == 9);
    REQUIRE(*std::max_element(correlated.begin(), correlated.end()) ==
            Catch::Approx(std::sqrt(2.0f)));
    REQUIRE(*std::max_element(correlated.begin(), correlated.end()) <= std::sqrt(2.0f) + 1.0e-6f);

    Transformer omitted;
    REQUIRE(omitted.prepare(1000.0, {3, 2, 1.0}));
    Transformer::OperationProgram omission;
    omission.steps = {{Transformer::Operation::Pass},
                      {Transformer::Operation::Omit},
                      {Transformer::Operation::Pass}};
    REQUIRE(omitted.set_program(0, omission));
    omitted.set_crossfade_law(pulp::signal::CrossfadeGainLaw::EqualGain);
    omitted.set_crossfade_duration_ms(20.0f);
    REQUIRE(drain(omitted, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f}) ==
            std::vector<float>{1.0f, 5.0f, 6.0f});

    Transformer reversed;
    REQUIRE(reversed.prepare(1000.0, {2, 4, 1.0}));
    REQUIRE(reversed.set_program(0, program(Transformer::Operation::Reverse)));
    reversed.set_crossfade_law(pulp::signal::CrossfadeGainLaw::EqualGain);
    reversed.set_crossfade_duration_ms(20.0f);
    REQUIRE(drain(reversed, {1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f}) ==
            std::vector<float>{4.0f, 3.0f, 2.0f, 7.0f, 6.0f, 5.0f});

    Transformer repeat_three;
    REQUIRE(repeat_three.prepare(1000.0, {2, 8, 1.0}));
    REQUIRE(repeat_three.set_program(0, program(Transformer::Operation::Repeat, 3)));
    repeat_three.set_crossfade_law(pulp::signal::CrossfadeGainLaw::EqualGain);
    repeat_three.set_crossfade_duration_ms(4.0f);
    const std::vector<float> eight{1, 2, 3, 4, 5, 6, 7, 8};
    const auto repeat_three_output = drain(repeat_three, eight);
    REQUIRE(repeat_three_output.size() == 16);
    const float inner_high = 134.0f / 27.0f;
    const float inner_low = 109.0f / 27.0f;
    const std::vector<float> repeat_three_expected{
        1, 2, 3, 4, 5, inner_high, inner_low, 4, 5, inner_high, inner_low, 4, 5, 6, 7, 8};
    for (std::size_t i = 0; i < repeat_three_output.size(); ++i) {
        if (i == 5 || i == 6 || i == 9 || i == 10) {
            const auto expected = repeat_three_expected[i];
            REQUIRE((repeat_three_output[i] == expected ||
                     repeat_three_output[i] ==
                         std::nextafter(expected, -std::numeric_limits<float>::infinity()) ||
                     repeat_three_output[i] ==
                         std::nextafter(expected, std::numeric_limits<float>::infinity())));
        } else {
            REQUIRE(repeat_three_output[i] == repeat_three_expected[i]);
        }
    }
}

TEST_CASE("waveset finish and reset cover empty partial exact and repeated EOS",
          "[signal][waveset]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {2, 4, 1.0}));
    transformer.finish_input();
    REQUIRE(transformer.drained());
    transformer.finish_input();
    const float sample = 1.0f;
    REQUIRE(transformer.push(&sample, 1) == 0);

    transformer.reset();
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Pass)));
    REQUIRE(drain(transformer, {1.0f, 2.0f, 3.0f}) == std::vector<float>{1.0f, 2.0f, 3.0f});
    transformer.reset();
    REQUIRE(drain(transformer, {1.0f, 2.0f, 3.0f, 4.0f}) ==
            std::vector<float>{1.0f, 2.0f, 3.0f, 4.0f});
    transformer.reset();
    const float infinity = std::numeric_limits<float>::infinity();
    REQUIRE(transformer.push(&infinity, 1) == 0);
}

TEST_CASE("waveset setters reject invalid values without clamping or lifecycle mutation",
          "[signal][waveset]") {
    Transformer transformer;
    REQUIRE_FALSE(transformer.set_zero_crossing_epsilon(0.0f));
    REQUIRE(transformer.prepare(1000.0, {4, 4, 2.0}));
    REQUIRE_FALSE(transformer.set_zero_crossing_polarity(
        static_cast<Transformer::ZeroCrossingPolarity>(255)));
    REQUIRE_FALSE(transformer.set_zero_crossing_epsilon(-1.0f));
    REQUIRE_FALSE(transformer.set_crossfade_law(static_cast<pulp::signal::CrossfadeGainLaw>(255)));
    REQUIRE_FALSE(transformer.set_crossfade_duration_ms(20.01f));
    REQUIRE_FALSE(transformer.set_normalize_ratio(2.01f));
    REQUIRE_FALSE(transformer.set_normalize_ratio(std::numeric_limits<float>::quiet_NaN()));
    REQUIRE(transformer.set_crossfade_duration_ms(20.0f));
    REQUIRE(transformer.set_normalize_ratio(2.0f));
    transformer.finish_input();
    REQUIRE_FALSE(transformer.set_coordinate_seed(1));
    REQUIRE_FALSE(transformer.request_program_slot(0));
}

TEST_CASE("waveset pending splice suffix expires at N omitted completions", "[signal][waveset]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Pass)));
    REQUIRE(transformer.set_program(1, program(Transformer::Operation::Omit)));
    transformer.set_crossfade_law(pulp::signal::CrossfadeGainLaw::EqualGain);
    transformer.set_crossfade_duration_ms(20.0f);
    const std::array<float, 10> input{1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f, 9.0f, 10.0f};
    REQUIRE(transformer.push(input.data(), 1) == 1);
    REQUIRE(transformer.request_program_slot(1));
    REQUIRE(transformer.push(input.data() + 1, 9) == 9);
    std::array<float, 4> output{};
    REQUIRE(transformer.pull(output.data(), 4) == 2);
    REQUIRE(output[0] == 1.0f);
    REQUIRE(output[1] == 2.0f);
    transformer.finish_input();
    REQUIRE(transformer.drained());
}

TEST_CASE("waveset pending predecessor CoordinateSelect Omit expires exactly at N",
          "[signal][waveset][splice]") {
    Transformer transformer;
    constexpr int n = 3;
    constexpr int m = 2;
    REQUIRE(transformer.prepare(1000.0, {n, m, 1.0}));
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Pass)));
    auto coordinate_omit = program(Transformer::Operation::CoordinateSelect);
    coordinate_omit.steps[0].coordinate_choice_count = 1;
    coordinate_omit.steps[0].coordinate_choices[0] = Transformer::Operation::Omit;
    REQUIRE(transformer.set_program(1, coordinate_omit));
    REQUIRE(transformer.set_crossfade_duration_ms(20.0f));
    const std::array<float, 2 * m + n * m> input{1, 2, 3, 4, 5, 6, 7, 8, 9, 10};
    REQUIRE(transformer.push(input.data(), m) == m);
    REQUIRE(transformer.request_program_slot(1));
    REQUIRE(transformer.push(input.data() + m, m) == m); // switching Pass
    std::array<float, 8> output{};
    REQUIRE(transformer.pull(output.data(), 8) == 2 * m - 1);
    for (int completion = 0; completion < n - 1; ++completion) {
        REQUIRE(transformer.push(input.data() + 2 * m + completion * m, m) == m);
        REQUIRE(transformer.pull(output.data(), 8) == 0);
    }
    REQUIRE(transformer.push(input.data() + 2 * m + (n - 1) * m, m) == m);
    REQUIRE(transformer.pull(output.data(), 8) == 1);
    REQUIRE(output[0] == 4.0f);
}

TEST_CASE("waveset nonidentity Rotate and Normalize edited splice oracles",
          "[signal][waveset][splice]") {
    Transformer rotate;
    REQUIRE(rotate.prepare(1000.0, {4, 2, 1.0}));
    Transformer::OperationProgram rotation;
    rotation.steps = {{Transformer::Operation::Rotate}, {Transformer::Operation::Rotate}};
    rotation.rotate_window = 2;
    rotation.permutation = {1, 0};
    REQUIRE(rotate.set_program(0, rotation));
    REQUIRE(rotate.set_crossfade_law(pulp::signal::CrossfadeGainLaw::EqualGain));
    REQUIRE(rotate.set_crossfade_duration_ms(2.0f));
    REQUIRE(drain(rotate, {1, 2, 3, 4}) == std::vector<float>{3, 1, 2});

    Transformer normalize;
    REQUIRE(normalize.prepare(1000.0, {2, 4, 2.0}));
    REQUIRE(normalize.set_program(0, program(Transformer::Operation::Normalize)));
    REQUIRE(normalize.set_normalize_ratio(2.0f));
    REQUIRE(normalize.set_crossfade_law(pulp::signal::CrossfadeGainLaw::EqualGain));
    REQUIRE(normalize.set_crossfade_duration_ms(2.0f));
    const auto normalized = drain(normalize, {1, 2, 3, 4, 5, 6, 7, 8});
    REQUIRE(normalized.size() == 14);
    REQUIRE(normalized.front() == 1.0f);
    REQUIRE(normalized.back() == 8.0f);
    REQUIRE(normalized[7] == Catch::Approx(38.0f / 7.0f));
}

TEST_CASE("waveset publication validation fails closed for malformed programs",
          "[signal][waveset]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {8, 2, 2.0}));
    for (std::uint8_t slot = 0; slot < Transformer::kMaxProgramSlots; ++slot) {
        auto owned = program(Transformer::Operation::Repeat, static_cast<std::uint8_t>(slot + 1u));
        REQUIRE(transformer.set_program(slot, owned));
        owned.steps.clear();
    }
    auto invalid = program(Transformer::Operation::Repeat, 0);
    REQUIRE_FALSE(transformer.set_program(0, invalid));
    invalid = program(Transformer::Operation::CoordinateSelect);
    invalid.steps[0].coordinate_choice_count = 1;
    invalid.steps[0].coordinate_choices[0] = Transformer::Operation::Rotate;
    REQUIRE_FALSE(transformer.set_program(0, invalid));
    invalid = program(static_cast<Transformer::Operation>(255));
    REQUIRE_FALSE(transformer.set_program(0, invalid));

    for (std::uint8_t slot = 0; slot < Transformer::kMaxProgramSlots; ++slot) {
        transformer.reset();
        REQUIRE(transformer.request_program_slot(slot));
        const auto output = drain(transformer, {1.0f, 2.0f, 3.0f, 4.0f});
        REQUIRE(output.size() == 4u * static_cast<std::size_t>(slot + 1u));
    }
}

TEST_CASE("waveset output is byte-identical across input partitions", "[signal][waveset]") {
    std::vector<float> input(53);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>((i % 11) + 1);
    auto coordinate = program(Transformer::Operation::CoordinateSelect, 3);
    coordinate.steps[0].coordinate_choice_count = 3;
    coordinate.steps[0].coordinate_choices[0] = Transformer::Operation::Pass;
    coordinate.steps[0].coordinate_choices[1] = Transformer::Operation::Repeat;
    coordinate.steps[0].coordinate_choices[2] = Transformer::Operation::Reverse;

    std::vector<float> reference;
    for (const int partition : {1, 7, 64, 4096}) {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {8, 8, 2.0}));
        REQUIRE(transformer.set_program(0, coordinate));
        transformer.set_coordinate_seed(0xabcdefu);
        auto output = drain(transformer, input, partition);
        if (reference.empty())
            reference = std::move(output);
        else
            REQUIRE(output == reference);
    }

    Transformer random_partition;
    REQUIRE(random_partition.prepare(1000.0, {8, 8, 2.0}));
    REQUIRE(random_partition.set_program(0, coordinate));
    random_partition.set_coordinate_seed(0xabcdefu);
    REQUIRE(drain_partitioned(random_partition, input, 0x31415926u) == reference);
}

TEST_CASE("waveset fixed milestones preserve constant-pull backpressure positions",
          "[signal][waveset][bounds]") {
    const auto reference = run_constant_pull_schedule(1);
    REQUIRE_FALSE(reference.backpressure_positions.empty());
    for (const int partition : {8, 64, 4096}) {
        const auto candidate = run_constant_pull_schedule(partition);
        REQUIRE(candidate.output == reference.output);
        REQUIRE(candidate.backpressure_positions == reference.backpressure_positions);
    }
    const auto random = run_constant_pull_schedule(1, 0x31415926u);
    REQUIRE(random.output == reference.output);
    REQUIRE(random.backpressure_positions == reference.backpressure_positions);
}

TEST_CASE("waveset slot requests latch at legal boundaries", "[signal][waveset]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {4, 4, 1.0}));
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Pass)));
    REQUIRE(transformer.set_program(7, program(Transformer::Operation::Reverse)));
    REQUIRE_FALSE(transformer.request_program_slot(8));
    const std::array<float, 2> first{-2.0f, -1.0f};
    REQUIRE(transformer.push(first.data(), 2) == 2);
    REQUIRE(transformer.request_program_slot(7));
    const std::array<float, 2> second{1.0f, 2.0f};
    REQUIRE(transformer.push(second.data(), 2) == 2);
    transformer.finish_input();
    std::array<float, 8> output{};
    REQUIRE(transformer.pull(output.data(), 8) == 4);
    REQUIRE(std::equal(output.begin(), output.begin() + 4,
                       std::array<float, 4>{-2.0f, -1.0f, 2.0f, 1.0f}.begin()));
}

TEST_CASE("waveset all eight slots switch at forced boundaries with exact retained programs",
          "[signal][waveset]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {8, 2, 1.0}));
    for (std::uint8_t slot = 0; slot < Transformer::kMaxProgramSlots; ++slot) {
        auto temporary =
            program(Transformer::Operation::Repeat, static_cast<std::uint8_t>(slot + 1u));
        REQUIRE(transformer.set_program(slot, temporary));
        temporary.steps[0].operation = Transformer::Operation::Omit;
    }
    for (std::uint8_t slot = 0; slot < Transformer::kMaxProgramSlots; ++slot) {
        transformer.reset();
        REQUIRE(transformer.request_program_slot(slot));
        const std::vector<float> segment{static_cast<float>(slot * 2u + 1u),
                                         static_cast<float>(slot * 2u + 2u)};
        std::vector<float> expected;
        for (std::uint8_t copy = 0; copy <= slot; ++copy)
            expected.insert(expected.end(), segment.begin(), segment.end());
        REQUIRE(drain(transformer, segment) == expected);
    }
    transformer.reset();
    auto malformed = program(Transformer::Operation::Repeat, 0);
    REQUIRE_FALSE(transformer.set_program(7, malformed));
    REQUIRE(transformer.request_program_slot(7));
    const std::vector<float> marker{31.0f, 32.0f};
    std::vector<float> retained;
    for (int copy = 0; copy < 8; ++copy)
        retained.insert(retained.end(), marker.begin(), marker.end());
    REQUIRE(drain(transformer, marker) == retained);
}

TEST_CASE("waveset rejected first push leaves program publication quiescent", "[signal][waveset]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {2, 2, 1.0}));
    const float infinity = std::numeric_limits<float>::infinity();
    REQUIRE(transformer.push(&infinity, 1) == 0);
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Reverse)));
    REQUIRE(transformer.push(nullptr, 1) == 0);
    REQUIRE(transformer.set_program(1, program(Transformer::Operation::Pass)));
    REQUIRE(drain(transformer, {1.0f, 2.0f}) == std::vector<float>{2.0f, 1.0f});
}

TEST_CASE("waveset rejected controls preserve rendered bytes", "[signal][waveset]") {
    Transformer control;
    Transformer rejected;
    REQUIRE(control.prepare(1000.0, {4, 4, 2.0}));
    REQUIRE(rejected.prepare(1000.0, {4, 4, 2.0}));
    REQUIRE(control.set_program(0, program(Transformer::Operation::Normalize)));
    REQUIRE(rejected.set_program(0, program(Transformer::Operation::Normalize)));
    REQUIRE(control.set_normalize_ratio(2.0f));
    REQUIRE(rejected.set_normalize_ratio(2.0f));
    REQUIRE_FALSE(rejected.set_normalize_ratio(2.001f));
    REQUIRE_FALSE(rejected.set_crossfade_duration_ms(-1.0f));
    REQUIRE_FALSE(rejected.set_zero_crossing_epsilon(-1.0f));
    const std::vector<float> input{-2.0f, -1.0f, 1.0f, 2.0f, -2.0f, -1.0f, 1.0f, 2.0f};
    REQUIRE(drain(rejected, input) == drain(control, input));
}

TEST_CASE("waveset normalize capacity comparison does not round upward through float",
          "[signal][waveset][bounds]") {
    Transformer transformer;
    constexpr double limit = 16.000000954;
    REQUIRE(transformer.prepare(1000.0, {2, 2, limit}));
    const float accepted = 16.0f;
    const float rejected = std::nextafter(accepted, std::numeric_limits<float>::infinity());
    REQUIRE(static_cast<double>(rejected) > limit);
    REQUIRE(transformer.set_normalize_ratio(accepted));
    REQUIRE_FALSE(transformer.set_normalize_ratio(rejected));
}

TEST_CASE("W=N forced Rotate saturation owns and releases each token exactly once",
          "[signal][waveset][bounds]") {
    Transformer transformer;
    constexpr int n = 4;
    REQUIRE(transformer.prepare(1000.0, {n, 1, 1.0}));
    Transformer::OperationProgram rotate;
    rotate.steps.resize(n);
    rotate.permutation = {0, 1, 2, 3};
    rotate.rotate_window = n;
    for (auto& step : rotate.steps)
        step.operation = Transformer::Operation::Rotate;
    REQUIRE(transformer.set_program(0, rotate));
    std::vector<float> input(160);
    for (std::size_t i = 0; i < input.size(); ++i)
        input[i] = static_cast<float>(i + 1u);
    std::vector<float> output;
    std::array<float, 3> block{};
    std::size_t offset = 0;
    bool saw_backpressure = false;
    while (offset < input.size()) {
        const int pushed =
            transformer.push(input.data() + offset, static_cast<int>(input.size() - offset));
        offset += static_cast<std::size_t>(pushed);
        const int pulled = transformer.pull(block.data(), static_cast<int>(block.size()));
        output.insert(output.end(), block.begin(), block.begin() + pulled);
        saw_backpressure = saw_backpressure || pushed == 0;
        REQUIRE((pushed > 0 || pulled > 0));
    }
    REQUIRE(saw_backpressure);
    transformer.finish_input();
    transformer.finish_input();
    while (!transformer.drained()) {
        const int pulled = transformer.pull(block.data(), static_cast<int>(block.size()));
        REQUIRE(pulled > 0);
        output.insert(output.end(), block.begin(), block.begin() + pulled);
    }
    REQUIRE(output == input);
    REQUIRE(transformer.push(input.data(), 1) == 0);
    transformer.reset();
    REQUIRE(drain(transformer, {1, 2, 3, 4}) == std::vector<float>{1, 2, 3, 4});

    Transformer repeat16;
    REQUIRE(repeat16.prepare(1000.0, {1, 2, 1.0}));
    REQUIRE(repeat16.set_program(0, program(Transformer::Operation::Repeat, 16)));
    REQUIRE(repeat16.latency_samples() == 0);
    REQUIRE(repeat16.max_lookahead_samples() == 4);
    REQUIRE(repeat16.tail_samples() == 84);
    REQUIRE(drain(repeat16, {1, 2, 3, 4, 5, 6}).size() == 96);
}

TEST_CASE("finish matrix drains within T", "[signal][waveset][eos]") {
    auto finish_and_count = [](Transformer& transformer, const float* input, int count) {
        if (count != 0)
            REQUIRE(transformer.push(input, count) == count);
        transformer.finish_input();
        transformer.finish_input();
        REQUIRE(transformer.push(input, count == 0 ? 0 : 1) == 0);
        std::array<float, 64> block{};
        std::size_t pulled_total = 0;
        while (!transformer.drained()) {
            const int pulled = transformer.pull(block.data(), static_cast<int>(block.size()));
            REQUIRE(pulled > 0);
            pulled_total += static_cast<std::size_t>(pulled);
        }
        REQUIRE(pulled_total <= transformer.tail_samples());
        return pulled_total;
    };
    const std::array<float, 8> input{1, 2, 3, 4, 5, 6, 7, 8};

    SECTION("empty") {
        Transformer t;
        REQUIRE(t.prepare(1000.0, {4, 2, 1.0}));
        REQUIRE(finish_and_count(t, input.data(), 0) == 0);
    }
    SECTION("partial capture") {
        Transformer t;
        REQUIRE(t.prepare(1000.0, {4, 4, 1.0}));
        REQUIRE(finish_and_count(t, input.data(), 3) == 3);
    }
    SECTION("W-1 Rotate held") {
        Transformer t;
        REQUIRE(t.prepare(1000.0, {4, 2, 1.0}));
        Transformer::OperationProgram p;
        p.steps = {{Transformer::Operation::Rotate}, {Transformer::Operation::Rotate}};
        p.rotate_window = 2;
        p.permutation = {1, 0};
        REQUIRE(t.set_program(0, p));
        REQUIRE(finish_and_count(t, input.data(), 2) == 2);
    }
    SECTION("just completed Rotate") {
        Transformer t;
        REQUIRE(t.prepare(1000.0, {4, 2, 1.0}));
        Transformer::OperationProgram p;
        p.steps = {{Transformer::Operation::Rotate}, {Transformer::Operation::Rotate}};
        p.rotate_window = 2;
        p.permutation = {1, 0};
        REQUIRE(t.set_program(0, p));
        REQUIRE(finish_and_count(t, input.data(), 4) == 4);
    }
    SECTION("maximal reachable unread state with outstanding credit and holdback") {
        Transformer t;
        REQUIRE(t.prepare(1000.0, {1, 2, 1.0}));
        REQUIRE(t.set_program(0, program(Transformer::Operation::Repeat, 16)));
        // Q also reserves one future E credit plus F splice capacity, so literal U==Q
        // is intentionally unreachable. This is the maximal unread-output admission edge.
        REQUIRE(t.push(input.data(), 6) == 4);
        std::array<float, 1> blocked{};
        REQUIRE(t.push(input.data() + 4, 2) == 0);
        REQUIRE(t.pull(blocked.data(), 1) == 1);
        REQUIRE(finish_and_count(t, input.data() + 4, 0) <= t.tail_samples());
    }
    SECTION("pending suffix") {
        Transformer t;
        REQUIRE(t.prepare(1000.0, {4, 2, 1.0}));
        REQUIRE(t.set_program(0, program(Transformer::Operation::Pass)));
        REQUIRE(t.set_program(1, program(Transformer::Operation::Omit)));
        REQUIRE(t.set_crossfade_duration_ms(20.0f));
        REQUIRE(t.push(input.data(), 2) == 2);
        REQUIRE(t.request_program_slot(1));
        REQUIRE(t.push(input.data() + 2, 2) == 2); // boundary-latched Pass
        REQUIRE(t.push(input.data() + 4, 2) == 2); // first active Omit
        std::array<float, 8> output{};
        REQUIRE(t.pull(output.data(), 8) == 3);
        REQUIRE(
            std::equal(output.begin(), output.begin() + 3, std::array<float, 3>{1, 2, 3}.begin()));
        t.finish_input();
        t.finish_input();
        REQUIRE(t.pull(output.data() + 3, 5) == 1);
        REQUIRE(output[3] == 4.0f);
        REQUIRE(t.drained());
        REQUIRE(4u <= t.tail_samples());
    }
}

TEST_CASE("splice expiry exact N", "[signal][waveset][splice]") {
    SECTION("all-Omit") {
        Transformer t;
        REQUIRE(t.prepare(1000.0, {4, 2, 1.0}));
        REQUIRE(t.set_program(0, program(Transformer::Operation::Omit)));
        REQUIRE(drain(t, {1, 2, 3, 4, 5, 6, 7, 8}).empty());
    }
    SECTION("CoordinateSelect-to-Omit frozen choice") {
        Transformer t;
        REQUIRE(t.prepare(1000.0, {4, 2, 1.0}));
        auto p = program(Transformer::Operation::CoordinateSelect);
        p.steps[0].coordinate_choice_count = 1;
        p.steps[0].coordinate_choices[0] = Transformer::Operation::Omit;
        REQUIRE(t.set_program(0, p));
        REQUIRE(t.set_coordinate_seed(0x1234));
        REQUIRE(drain(t, {1, 2, 3, 4, 5, 6, 7, 8}).empty());
    }
    SECTION("output-producing segment exactly N splices") {
        Transformer t;
        REQUIRE(t.prepare(1000.0, {3, 2, 1.0}));
        Transformer::OperationProgram p;
        p.steps = {{Transformer::Operation::Pass},
                   {Transformer::Operation::Omit},
                   {Transformer::Operation::Omit}};
        REQUIRE(t.set_program(0, p));
        REQUIRE(t.set_program(1, program(Transformer::Operation::Pass)));
        REQUIRE(t.set_crossfade_law(pulp::signal::CrossfadeGainLaw::EqualGain));
        REQUIRE(t.set_crossfade_duration_ms(20.0f));
        const std::array<float, 6> first{1, 2, 3, 4, 5, 6};
        REQUIRE(t.push(first.data(), 6) == 6);
        REQUIRE(t.request_program_slot(1));
        REQUIRE(drain(t, {7, 8}) == std::vector<float>{1, 7, 8});
    }
}

TEST_CASE("capacity lookahead components A and G remain explicit", "[signal][waveset][latency]") {
    constexpr int n = 4;
    constexpr int m = 3;
    constexpr int wcap = 4;
    constexpr int a = wcap * m;
    constexpr int g = n * m;
    Transformer rotate;
    REQUIRE(rotate.prepare(1000.0, {n, m, 1.0}));
    REQUIRE(rotate.latency_samples() == 0);
    REQUIRE(rotate.max_lookahead_samples() == a + g);
    Transformer::OperationProgram p;
    p.steps.resize(wcap);
    p.permutation = {0, 1, 2, 3};
    p.rotate_window = wcap;
    for (auto& step : p.steps)
        step.operation = Transformer::Operation::Rotate;
    REQUIRE(rotate.set_program(0, p));
    const std::array<float, a> held{1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12};
    REQUIRE(rotate.push(held.data(), a - 1) == a - 1);
    std::array<float, 32> output{};
    REQUIRE(rotate.pull(output.data(), 32) == 0);
    REQUIRE(rotate.push(held.data() + a - 1, 1) == 1);
    // One-sample live splice holdback remains until EOS.
    REQUIRE(rotate.pull(output.data(), 32) == a - 1);
    rotate.finish_input();
    REQUIRE(rotate.pull(output.data() + a - 1, 1) == 1);

    constexpr int gm = 4;
    constexpr int exact_g = n * gm;
    Transformer omit;
    REQUIRE(omit.prepare(1000.0, {n, gm, 1.0}));
    REQUIRE(omit.set_program(0, program(Transformer::Operation::Pass)));
    REQUIRE(omit.set_program(1, program(Transformer::Operation::Omit)));
    std::array<float, 2 * gm + exact_g> omission_input{};
    for (std::size_t i = 0; i < omission_input.size(); ++i)
        omission_input[i] = static_cast<float>(i + 1u);
    REQUIRE(omit.push(omission_input.data(), gm) == gm);
    REQUIRE(omit.request_program_slot(1));
    REQUIRE(omit.push(omission_input.data() + gm, gm) == gm); // switching Pass segment
    REQUIRE(omit.pull(output.data(), 32) == 2 * gm - 2);
    REQUIRE(std::equal(output.begin(), output.begin() + 2 * gm - 2, omission_input.begin()));
    for (int completion = 0; completion < n - 1; ++completion) {
        REQUIRE(omit.push(omission_input.data() + 2 * gm + completion * gm, gm) == gm);
        REQUIRE(omit.pull(output.data(), 32) == 0);
    }
    // Exactly G=N*M omission input after activation releases the held suffix.
    REQUIRE(omit.push(omission_input.data() + 2 * gm + (n - 1) * gm, gm) == gm);
    REQUIRE(omit.pull(output.data(), 32) == 2);
    REQUIRE(output[0] == omission_input[2 * gm - 2]);
    REQUIRE(output[1] == omission_input[2 * gm - 1]);
    REQUIRE(exact_g == n * gm);
}

TEST_CASE("waveset live operations are allocation free", "[signal][waveset][rt-safety]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {4, 8, 2.0}));
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Repeat, 2)));
    std::array<float, 8> input{-2.0f, -1.0f, 1.0f, 2.0f, -2.0f, -1.0f, 1.0f, 2.0f};
    std::array<float, 128> output{};
    bool requested = false;
    bool polarity_set = false, epsilon_set = false, law_set = false, fade_set = false;
    bool ratio_set = false, seed_set = false, prepared = false, drained_state = false;
    int latency = 0, max_lookahead = 0;
    std::size_t tail = 0;
    int accepted = 0;
    bool allocated = true;
    {
        pulp::test::RtAllocationProbe probe;
        polarity_set =
            transformer.set_zero_crossing_polarity(Transformer::ZeroCrossingPolarity::Both);
        epsilon_set = transformer.set_zero_crossing_epsilon(0.0f);
        law_set = transformer.set_crossfade_law(pulp::signal::CrossfadeGainLaw::EqualGain);
        fade_set = transformer.set_crossfade_duration_ms(2.0f);
        ratio_set = transformer.set_normalize_ratio(2.0f);
        seed_set = transformer.set_coordinate_seed(7);
        prepared = transformer.prepared();
        latency = transformer.latency_samples();
        max_lookahead = transformer.max_lookahead_samples();
        tail = transformer.tail_samples();
        requested = transformer.request_program_slot(0);
        accepted = transformer.push(input.data(), static_cast<int>(input.size()));
        (void)transformer.pull(output.data(), static_cast<int>(output.size()));
        transformer.finish_input();
        (void)transformer.pull(output.data(), static_cast<int>(output.size()));
        drained_state = transformer.drained();
        transformer.reset();
        allocated = probe.saw_allocation();
    }
    REQUIRE(requested);
    REQUIRE(polarity_set);
    REQUIRE(epsilon_set);
    REQUIRE(law_set);
    REQUIRE(fade_set);
    REQUIRE(ratio_set);
    REQUIRE(seed_set);
    REQUIRE(prepared);
    REQUIRE(latency == 0);
    REQUIRE(max_lookahead == 64);
    REQUIRE(tail == 660);
    REQUIRE(drained_state);
    REQUIRE(accepted == static_cast<int>(input.size()));
    REQUIRE_FALSE(allocated);
}

TEST_CASE("RT full public surface every operation at capacity", "[signal][waveset][rt-safety]") {
    static_assert(std::atomic<std::uint32_t>::is_always_lock_free);
    static_assert(noexcept(std::declval<Transformer&>().push(nullptr, 0)));
    static_assert(noexcept(std::declval<Transformer&>().pull(nullptr, 0)));
    static_assert(noexcept(std::declval<Transformer&>().finish_input()));
    static_assert(noexcept(std::declval<Transformer&>().reset()));
    const std::array operations{
        Transformer::Operation::Pass,      Transformer::Operation::Repeat,
        Transformer::Operation::Omit,      Transformer::Operation::Reverse,
        Transformer::Operation::Normalize, Transformer::Operation::CoordinateSelect};
    for (const auto operation : operations) {
        Transformer transformer;
        REQUIRE(transformer.prepare(1000.0, {8, 8, 2.0}));
        auto p = program(operation, 2);
        if (operation == Transformer::Operation::CoordinateSelect) {
            p.steps[0].coordinate_choice_count = 2;
            p.steps[0].coordinate_choices[0] = Transformer::Operation::Pass;
            p.steps[0].coordinate_choices[1] = Transformer::Operation::Omit;
        }
        REQUIRE(transformer.set_program(0, p));
        std::array<float, 64> input{};
        for (std::size_t i = 0; i < input.size(); ++i)
            input[i] = (i & 1u) ? 1.0f : -1.0f;
        std::array<float, 2048> output{};
        bool allocated = true;
        {
            pulp::test::RtAllocationProbe probe;
            REQUIRE(
                transformer.set_zero_crossing_polarity(Transformer::ZeroCrossingPolarity::Both));
            REQUIRE(transformer.set_zero_crossing_epsilon(0.0f));
            REQUIRE(transformer.set_crossfade_law(pulp::signal::CrossfadeGainLaw::EqualGain));
            REQUIRE(transformer.set_crossfade_duration_ms(2.0f));
            REQUIRE(transformer.set_normalize_ratio(2.0f));
            REQUIRE(transformer.set_coordinate_seed(9));
            REQUIRE(transformer.request_program_slot(0));
            REQUIRE(transformer.prepared());
            REQUIRE(transformer.latency_samples() == 0);
            REQUIRE(transformer.max_lookahead_samples() > 0);
            REQUIRE(transformer.tail_samples() > 0);
            REQUIRE(transformer.push(input.data(), static_cast<int>(input.size())) > 0);
            (void)transformer.pull(output.data(), static_cast<int>(output.size()));
            transformer.finish_input();
            (void)transformer.drained();
            transformer.reset();
            allocated = probe.saw_allocation();
        }
        REQUIRE_FALSE(allocated);
    }

    Transformer rotate;
    REQUIRE(rotate.prepare(1000.0, {8, 1, 1.0}));
    Transformer::OperationProgram p;
    p.steps.resize(8);
    p.permutation = {0, 1, 2, 3, 4, 5, 6, 7};
    p.rotate_window = 8;
    for (auto& step : p.steps)
        step.operation = Transformer::Operation::Rotate;
    REQUIRE(rotate.set_program(0, p));
    std::array<float, 8> input{1, 2, 3, 4, 5, 6, 7, 8};
    bool allocated = true;
    {
        pulp::test::RtAllocationProbe probe;
        REQUIRE(rotate.push(input.data(), 8) == 8);
        rotate.finish_input();
        rotate.reset();
        allocated = probe.saw_allocation();
    }
    REQUIRE_FALSE(allocated);
}
