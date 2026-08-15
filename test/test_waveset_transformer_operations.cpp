#include "support/waveset_transformer_test_support.hpp"

using namespace pulp::test::waveset;

TEST_CASE("waveset transformer validates preparation and publishes owned programs",
          "[signal][waveset]") {
    Transformer transformer;
    REQUIRE_FALSE(transformer.prepare(0.0, {2, 8, 2.0}));
    REQUIRE_FALSE(transformer.prepare(48000.0, {0, 8, 2.0}));
    REQUIRE_FALSE(transformer.prepare(48000.0, {2, 0, 2.0}));
    REQUIRE_FALSE(transformer.prepare(48000.0, {2, 8, 0.5}));
    REQUIRE_FALSE(transformer.prepare(std::numeric_limits<double>::infinity(), {2, 8, 2.0}));
    REQUIRE_FALSE(transformer.prepare(
        48000.0, {std::numeric_limits<int>::max(), std::numeric_limits<int>::max(), 16.0}));
    REQUIRE(transformer.prepare(1000.0, {4, 8, 2.0}));
    REQUIRE(transformer.prepared());
    REQUIRE_FALSE(transformer.prepare(2000.0, {2, 4, 1.0}));
    REQUIRE(transformer.latency_samples() == 0);
    REQUIRE(transformer.max_lookahead_samples() == 64);
    REQUIRE(transformer.tail_samples() == 660);

    auto temporary = program(Transformer::Operation::Reverse);
    REQUIRE(transformer.set_program(0, temporary));
    temporary.steps[0].operation = Transformer::Operation::Omit;
    temporary.steps.clear();
    REQUIRE(drain(transformer, {-2.0f, -1.0f, 1.0f, 2.0f}) ==
            std::vector<float>{-1.0f, -2.0f, 2.0f, 1.0f});
    REQUIRE_FALSE(transformer.set_program(0, program(Transformer::Operation::Pass)));
    REQUIRE(transformer.push(nullptr, 1) == 0);
    transformer.reset();
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Pass)));
}

TEST_CASE("waveset CapacityLayout rejects every checked arithmetic overflow stage",
          "[signal][waveset][bounds]") {
    using Layout = pulp::signal::detail::CapacityLayout;
    constexpr auto maximum = std::numeric_limits<std::size_t>::max();
    std::size_t value{};
    REQUIRE_FALSE(Layout::add(maximum, 1, value));
    REQUIRE_FALSE(Layout::multiply(maximum, 2, value));

    auto makes = [](double sample_rate, std::size_t n, std::size_t m, double normalize_ratio,
                    std::size_t repeat_limit, std::size_t rotate_limit, std::size_t slot_count,
                    std::size_t sample_bytes = sizeof(float),
                    std::size_t held_bytes = sizeof(std::size_t) * 3, std::size_t token_bytes = 1,
                    std::size_t program_bytes = sizeof(std::size_t) * 2,
                    std::size_t step_bytes = 16) {
        Layout layout{};
        return Layout::make(sample_rate, n, m, normalize_ratio, repeat_limit, rotate_limit,
                            slot_count, sample_bytes, held_bytes, token_bytes, program_bytes,
                            step_bytes, layout);
    };
    REQUIRE(makes(1000.0, 4, 8, 2.0, 16, 256, 8));
    REQUIRE_FALSE(makes(1000.0, 4, maximum, 1.0, 2, 256, 8)); // repeat M*16
    REQUIRE_FALSE(makes(1000.0, 4, maximum, 2.0, 1, 256, 8)); // Normalize M*R
    REQUIRE_FALSE(makes(std::numeric_limits<double>::max(), 4, 8, 1.0, 1, 256, 8));
    REQUIRE_FALSE(makes(1.0, maximum, 1, 1.0, 1, 1, 1));               // N+1
    REQUIRE_FALSE(makes(1.0, maximum / 2, 1, 1.0, 4, 1, 1));           // (N+1)*E / Q
    REQUIRE_FALSE(makes(1.0, maximum / 2 + 1, 2, 1.0, 1, maximum, 1)); // A/G
    REQUIRE_FALSE(makes(1.0, 2, 1, 1.0, 1, 1, maximum, 1, 1, maximum, 1));
    REQUIRE_FALSE(makes(1.0, 2, 1, 1.0, 1, 2, maximum, 1, 1, 1, 1));
    REQUIRE_FALSE(makes(1.0, 2, 1, 1.0, 1, 2, 1, maximum, 1, 1, 1));
}

TEST_CASE("waveset segmentation honors polarity deadband and forced boundaries",
          "[signal][waveset]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {8, 4, 1.0}));
    transformer.set_zero_crossing_polarity(Transformer::ZeroCrossingPolarity::Both);
    transformer.set_zero_crossing_epsilon(0.25f);
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Reverse)));
    const std::vector<float> input{-2.0f, -0.25f, 0.25f, 2.0f, 2.0f, 0.0f, -2.0f, -1.0f};
    const std::vector<float> expected{0.25f, -0.25f, -2.0f, 0.0f, 2.0f, 2.0f, -1.0f, -2.0f};
    REQUIRE(drain(transformer, input, 1) == expected);

    transformer.reset();
    transformer.set_zero_crossing_polarity(Transformer::ZeroCrossingPolarity::Rising);
    transformer.set_zero_crossing_epsilon(0.0f);
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Pass)));
    const std::vector<float> dc(17, 0.5f);
    REQUIRE(drain(transformer, dc) == dc);
    transformer.reset();
    const std::vector<float> silence(17, 0.0f);
    REQUIRE(drain(transformer, silence) == silence);

    Transformer falling;
    REQUIRE(falling.prepare(1000.0, {8, 8, 1.0}));
    falling.set_zero_crossing_polarity(Transformer::ZeroCrossingPolarity::Falling);
    falling.set_zero_crossing_epsilon(0.25f);
    REQUIRE(falling.set_program(0, program(Transformer::Operation::Reverse)));
    const std::vector<float> falling_input{2.0f,  1.0f, 0.25f, -0.25f, -1.0f,
                                           -2.0f, 1.0f, 2.0f,  -1.0f,  -2.0f};
    REQUIRE(drain(falling, falling_input, 3) ==
            std::vector<float>{-0.25f, 0.25f, 1.0f, 2.0f, 2.0f, 1.0f, -2.0f, -1.0f, -2.0f, -1.0f});
}

TEST_CASE("waveset primitive operations have exact sample oracles", "[signal][waveset]") {
    const std::vector<float> input{1.0f, 2.0f, 3.0f, 4.0f};
    for (const auto push_size : {1, 4, 64}) {
        Transformer pass;
        REQUIRE(pass.prepare(1000.0, {2, 16, 2.0}));
        REQUIRE(pass.set_program(0, program(Transformer::Operation::Pass)));
        pass.set_crossfade_law(pulp::signal::CrossfadeGainLaw::EqualPower);
        pass.set_crossfade_duration_ms(20.0f);
        REQUIRE(drain(pass, input, push_size) == input);
    }

    Transformer repeat;
    REQUIRE(repeat.prepare(1000.0, {2, 16, 2.0}));
    REQUIRE(repeat.set_program(0, program(Transformer::Operation::Repeat, 16)));
    const auto repeated = drain(repeat, input);
    REQUIRE(repeated.size() == input.size() * 16);
    for (std::size_t copy = 0; copy < 16; ++copy)
        REQUIRE(std::equal(input.begin(), input.end(), repeated.begin() + copy * input.size()));

    Transformer omitted;
    REQUIRE(omitted.prepare(1000.0, {2, 16, 2.0}));
    REQUIRE(omitted.set_program(0, program(Transformer::Operation::Omit)));
    REQUIRE(drain(omitted, input).empty());

    Transformer normalized;
    REQUIRE(normalized.prepare(1000.0, {2, 16, 2.0}));
    REQUIRE(normalized.set_program(0, program(Transformer::Operation::Normalize)));
    normalized.set_normalize_ratio(2.0f);
    const auto stretched = drain(normalized, {0.0f, 1.0f, 2.0f, 3.0f});
    REQUIRE(stretched.size() == 8);
    REQUIRE(stretched.front() == 0.0f);
    REQUIRE(stretched.back() == 3.0f);
    REQUIRE(stretched[1] == Catch::Approx(3.0f / 7.0f));

    normalized.reset();
    REQUIRE(normalized.set_program(0, program(Transformer::Operation::Normalize)));
    normalized.set_normalize_ratio(0.5f);
    REQUIRE(drain(normalized, input) == std::vector<float>{1.0f, 4.0f});
    normalized.reset();
    REQUIRE(normalized.set_program(0, program(Transformer::Operation::Normalize)));
    normalized.set_normalize_ratio(1.0f);
    REQUIRE(drain(normalized, input) == input);

    Transformer reverse_once;
    REQUIRE(reverse_once.prepare(1000.0, {2, 4, 1.0}));
    REQUIRE(reverse_once.set_program(0, program(Transformer::Operation::Reverse)));
    const auto reversed = drain(reverse_once, input);
    Transformer reverse_twice;
    REQUIRE(reverse_twice.prepare(1000.0, {2, 4, 1.0}));
    REQUIRE(reverse_twice.set_program(0, program(Transformer::Operation::Reverse)));
    REQUIRE(drain(reverse_twice, reversed) == input);
}

TEST_CASE("waveset Rotate reorders complete groups and preserves an incomplete tail",
          "[signal][waveset]") {
    Transformer transformer;
    REQUIRE(transformer.prepare(1000.0, {4, 2, 1.0}));
    Transformer::OperationProgram rotate;
    rotate.steps = {{Transformer::Operation::Rotate}, {Transformer::Operation::Rotate}};
    rotate.rotate_window = 2;
    rotate.permutation = {1, 0};
    REQUIRE(transformer.set_program(0, rotate));
    const auto output = drain(transformer, four_segments());
    REQUIRE(output == std::vector<float>{1.0f, 2.0f, -4.0f, -3.0f, 3.0f, 4.0f, -2.0f, -1.0f});

    Transformer inverse;
    REQUIRE(inverse.prepare(1000.0, {4, 2, 1.0}));
    REQUIRE(inverse.set_program(0, rotate));
    REQUIRE(drain(inverse, output) == four_segments());

    Transformer deferred_switch;
    REQUIRE(deferred_switch.prepare(1000.0, {4, 2, 1.0}));
    REQUIRE(deferred_switch.set_program(0, rotate));
    Transformer::OperationProgram next_program;
    next_program.steps = {{Transformer::Operation::Reverse}, {Transformer::Operation::Omit}};
    REQUIRE(deferred_switch.set_program(1, next_program));
    const std::array<float, 2> rotate_first{-4.0f, -3.0f};
    const std::array<float, 2> rotate_second{1.0f, 2.0f};
    const std::array<float, 2> after_switch{-2.0f, -1.0f};
    const std::array<float, 2> cursor_probe{3.0f, 4.0f};
    REQUIRE(deferred_switch.push(rotate_first.data(), 2) == 2);
    REQUIRE(deferred_switch.request_program_slot(1));
    REQUIRE(deferred_switch.push(rotate_second.data(), 2) == 2);
    REQUIRE(deferred_switch.push(after_switch.data(), 2) == 2);
    REQUIRE(deferred_switch.push(cursor_probe.data(), 2) == 2);
    deferred_switch.finish_input();
    std::array<float, 16> switched_output{};
    REQUIRE(deferred_switch.pull(switched_output.data(), 16) == 6);
    REQUIRE(std::equal(switched_output.begin(), switched_output.begin() + 6,
                       std::array<float, 6>{1.0f, 2.0f, -4.0f, -3.0f, -1.0f, -2.0f}.begin()));

    transformer.reset();
    REQUIRE(transformer.set_program(0, rotate));
    REQUIRE(drain(transformer, {-2.0f, -1.0f}) == std::vector<float>{-2.0f, -1.0f});

    transformer.reset();
    rotate.permutation = {0, 0};
    REQUIRE_FALSE(transformer.set_program(0, rotate));
    rotate.permutation = {0, 2};
    REQUIRE_FALSE(transformer.set_program(0, rotate));
    rotate.permutation = {0};
    REQUIRE_FALSE(transformer.set_program(0, rotate));
    rotate.steps = {{Transformer::Operation::Pass}, {Transformer::Operation::Rotate}};
    rotate.rotate_window = 2;
    rotate.permutation = {0, 1};
    REQUIRE_FALSE(transformer.set_program(0, rotate));
}

TEST_CASE("waveset coordinate selection is stateless and repeatable", "[signal][waveset]") {
    auto selected_program = program(Transformer::Operation::CoordinateSelect, 2);
    auto& step = selected_program.steps[0];
    step.coordinate_choice_count = 3;
    step.coordinate_choices[0] = Transformer::Operation::Pass;
    step.coordinate_choices[1] = Transformer::Operation::Reverse;
    step.coordinate_choices[2] = Transformer::Operation::Omit;

    Transformer first;
    Transformer second;
    for (auto* transformer : {&first, &second}) {
        REQUIRE(transformer->prepare(1000.0, {8, 2, 1.0}));
        REQUIRE(transformer->set_program(0, selected_program));
        transformer->set_coordinate_seed(0x12345678u);
    }
    const auto first_output = drain(first, four_segments(), 1);
    REQUIRE(first_output == drain(second, four_segments(), 8));
    std::vector<float> oracle;
    const auto input = four_segments();
    for (std::size_t segment = 0; segment < 4; ++segment) {
        const auto draw = oracle_coordinate(0x12345678u, segment);
        const auto choice = static_cast<std::size_t>(draw * 3.0);
        const auto begin = input.begin() + static_cast<std::ptrdiff_t>(segment * 2u);
        if (choice == 0)
            oracle.insert(oracle.end(), begin, begin + 2);
        else if (choice == 1)
            oracle.insert(oracle.end(), std::reverse_iterator(begin + 2),
                          std::reverse_iterator(begin));
    }
    REQUIRE(first_output == oracle);
}

TEST_CASE("waveset exact periodic segmentation and literal harmonic crossings are deterministic",
          "[signal][waveset]") {
    constexpr double pi = 3.14159265358979323846;
    std::vector<float> sine(350);
    for (std::size_t i = 0; i < sine.size(); ++i)
        sine[i] = (i % 100u) < 50u ? 1.0f : -1.0f;

    Transformer transformer;
    REQUIRE(transformer.prepare(44100.0, {8, 128, 1.0}));
    transformer.set_zero_crossing_polarity(Transformer::ZeroCrossingPolarity::Rising);
    transformer.set_zero_crossing_epsilon(1.0e-4f);
    REQUIRE(transformer.set_program(0, program(Transformer::Operation::Reverse)));
    std::vector<float> expected;
    for (std::size_t start :
         {std::size_t{0}, std::size_t{100}, std::size_t{200}, std::size_t{300}}) {
        const auto end = std::min(start + 100u, sine.size());
        expected.insert(expected.end(),
                        sine.rbegin() + static_cast<std::ptrdiff_t>(sine.size() - end),
                        sine.rbegin() + static_cast<std::ptrdiff_t>(sine.size() - start));
    }
    REQUIRE(drain(transformer, sine, 37) == expected);

    std::vector<float> harmonic(200);
    for (std::size_t i = 0; i < harmonic.size(); ++i) {
        const auto phase = 2.0 * pi * static_cast<double>(i) / 100.0;
        harmonic[i] = static_cast<float>(std::sin(phase) + 1.5 * std::sin(3.0 * phase));
    }
    int rising_crossings = 0;
    int last_side = 0;
    std::vector<std::size_t> boundaries;
    for (std::size_t index = 0; index < harmonic.size(); ++index) {
        const auto sample = harmonic[index];
        const int side = sample > 1.0e-4f ? 1 : (sample < -1.0e-4f ? -1 : 0);
        if (side == 1 && last_side == -1) {
            ++rising_crossings;
            boundaries.push_back(index);
        }
        if (side != 0)
            last_side = side;
    }
    REQUIRE(rising_crossings > 2);

    std::vector<float> harmonic_expected;
    std::size_t begin = 0;
    boundaries.push_back(harmonic.size());
    for (const auto end : boundaries) {
        harmonic_expected.insert(harmonic_expected.end(),
                                 std::reverse_iterator(harmonic.begin() + end),
                                 std::reverse_iterator(harmonic.begin() + begin));
        begin = end;
    }
    Transformer harmonic_transformer;
    REQUIRE(harmonic_transformer.prepare(44100.0, {16, 128, 1.0}));
    harmonic_transformer.set_zero_crossing_polarity(Transformer::ZeroCrossingPolarity::Rising);
    harmonic_transformer.set_zero_crossing_epsilon(1.0e-4f);
    REQUIRE(harmonic_transformer.set_program(0, program(Transformer::Operation::Reverse)));
    REQUIRE(drain(harmonic_transformer, harmonic, 29) == harmonic_expected);
}
