#include "character_engine_bank.hpp"
#include "delay_params.hpp"
#include "delay_time_model.hpp"
#include "pulp_delay.hpp"

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>
#include <pulp/audio/analysis/audio_assertions.hpp>
#include <pulp/audio/analysis/audio_metrics.hpp>
#include <pulp/format/headless.hpp>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <ctime>
#include <limits>

using Catch::Matchers::WithinAbs;
using namespace pulp;
using namespace pulp::examples::delay;

namespace {

void process(format::HeadlessHost& host, const audio::Buffer<float>& input,
             audio::Buffer<float>& output) {
    const float* input_channels[] = {
        input.channel(0).data(),
        input.channel(1).data(),
    };
    audio::BufferView<const float> input_view(input_channels, 2, input.num_samples());
    auto output_view = output.view();
    host.process(output_view, input_view);
}

float max_step(const audio::Buffer<float>& buffer, std::size_t channel,
               float previous_sample) {
    float result = 0.0f;
    for (const float sample : buffer.channel(channel)) {
        result = std::max(result, std::abs(sample - previous_sample));
        previous_sample = sample;
    }
    return result;
}

} // namespace

TEST_CASE("Pulp Delay registers its stable 25-parameter contract", "[pulp-delay][parameters]") {
    state::StateStore store;
    define_delay_parameters(store);

    REQUIRE(store.param_count() == kParameterCount);
    for (state::ParamID id = kTime; id <= kReverse; ++id) {
        INFO("parameter id " << id);
        REQUIRE(store.info(id) != nullptr);
        REQUIRE(store.info(id)->id == id);
    }

    REQUIRE(store.info(kCharacter)->kind == state::ParamKind::Enum);
    REQUIRE(store.info(kCharacter)->value_labels ==
            std::vector<std::string>{"Clean", "Vintage", "Tape", "BBD"});
    REQUIRE(store.info(kRouting)->kind == state::ParamKind::Enum);
    REQUIRE(store.info(kSync)->kind == state::ParamKind::Toggle);
    REQUIRE(store.info(kFreeze)->kind == state::ParamKind::Toggle);
    REQUIRE(store.info(kReverse)->kind == state::ParamKind::Toggle);

    REQUIRE_THAT(store.get_value(kTime), WithinAbs(380.0, 0.01));
    REQUIRE_THAT(store.get_value(kFeedback), WithinAbs(62.0, 0.001));
    REQUIRE_THAT(store.get_value(kCharacter), WithinAbs(2.0, 0.001));
    REQUIRE_THAT(store.get_value(kMix), WithinAbs(45.0, 0.001));
    REQUIRE_THAT(store.get_value(kLowCut), WithinAbs(180.0, 0.01));
    REQUIRE_THAT(store.get_value(kHighCut), WithinAbs(4800.0, 0.01));
    REQUIRE_THAT(store.info(kFeedback)->range.max, WithinAbs(110.0, 0.001));
    REQUIRE_THAT(store.info(kMix)->range.max, WithinAbs(100.0, 0.001));
}

TEST_CASE("Delay time model maps every authored division at 120 BPM", "[pulp-delay][timing]") {
    constexpr std::array<double, 11> expected_ms = {
        62.5, 83.333333, 125.0, 166.666667, 250.0, 375.0, 333.333333, 500.0, 750.0, 1000.0, 2000.0,
    };
    for (int index = 0; index < static_cast<int>(expected_ms.size()); ++index) {
        INFO("division " << kDivisionLabels[static_cast<std::size_t>(index)]);
        REQUIRE_THAT(DelayTimeModel::synced_time_ms(index, 120.0),
                     WithinAbs(expected_ms[static_cast<std::size_t>(index)], 0.01));
    }
    REQUIRE_THAT(DelayTimeModel::synced_time_ms(4, 0.0), WithinAbs(250.0, 0.01));
    REQUIRE_THAT(DelayTimeModel::synced_time_ms(4, std::numeric_limits<double>::quiet_NaN()),
                 WithinAbs(250.0, 0.01));
}

TEST_CASE("Delay time model selects exactly one right-time policy", "[pulp-delay][timing]") {
    DelayTimeInputs inputs;

    REQUIRE(DelayTimeModel::right_timing_branch(inputs)
            == RightTimingBranch::linked_ratio);
    auto linked_ratio = DelayTimeModel::derive(inputs);
    REQUIRE(linked_ratio.right_uses_ratio);
    REQUIRE_THAT(linked_ratio.left_ms, WithinAbs(380.0, 0.01));
    REQUIRE_THAT(linked_ratio.right_ms, WithinAbs(425.6, 0.01));

    inputs.offset_mode = OffsetMode::milliseconds;
    REQUIRE(DelayTimeModel::right_timing_branch(inputs)
            == RightTimingBranch::linked_offset_ms);
    auto linked_ms = DelayTimeModel::derive(inputs);
    REQUIRE_FALSE(linked_ms.right_uses_ratio);
    REQUIRE_THAT(linked_ms.right_ms, WithinAbs(394.0, 0.01));

    inputs.link = false;
    REQUIRE(DelayTimeModel::right_timing_branch(inputs)
            == RightTimingBranch::free_independent);
    auto split_free = DelayTimeModel::derive(inputs);
    REQUIRE_FALSE(split_free.right_uses_ratio);
    REQUIRE_THAT(split_free.right_ms, WithinAbs(620.0, 0.01));

    inputs.sync = true;
    inputs.division = 4;
    inputs.division_right = 6;
    inputs.tempo_bpm = 120.0;
    REQUIRE(DelayTimeModel::right_timing_branch(inputs)
            == RightTimingBranch::synced_independent);
    auto split_sync = DelayTimeModel::derive(inputs);
    REQUIRE_THAT(split_sync.left_ms, WithinAbs(250.0, 0.01));
    REQUIRE_THAT(split_sync.right_ms, WithinAbs(333.333, 0.01));

    inputs.routing = Routing::ping_pong;
    REQUIRE(DelayTimeModel::right_timing_branch(inputs)
            == RightTimingBranch::ping_pong);
    auto ping_pong = DelayTimeModel::derive(inputs);
    REQUIRE(ping_pong.right_uses_ratio);
    REQUIRE_THAT(ping_pong.right_ratio, WithinAbs(1.0, 0.001));
    REQUIRE_THAT(ping_pong.right_ms, WithinAbs(ping_pong.left_ms, 0.001));
}

TEST_CASE("Delay time inputs project store state with caller-owned tempo",
          "[pulp-delay][timing][projection]") {
    state::StateStore store;
    define_delay_parameters(store);
    store.set_value(kTime, 731.0f);
    store.set_value(kSync, 1.0f);
    store.set_value(kDivision, 8.0f);
    store.set_value(kLink, 0.0f);
    store.set_value(kOffsetMode,
                    static_cast<float>(OffsetMode::milliseconds));
    store.set_value(kTimeOffset, 0.75f);
    store.set_value(kOffsetMs, -27.0f);
    store.set_value(kTimeRight, 911.0f);
    store.set_value(kDivisionRight, 10.0f);
    store.set_value(kRouting, static_cast<float>(Routing::ping_pong));

    const auto inputs = delay_time_inputs_from_store(store, 137.5);
    REQUIRE_THAT(inputs.time_ms, WithinAbs(731.0, 0.001));
    REQUIRE(inputs.sync);
    REQUIRE(inputs.division == 8);
    REQUIRE_FALSE(inputs.link);
    REQUIRE(inputs.offset_mode == OffsetMode::milliseconds);
    REQUIRE_THAT(inputs.time_offset, WithinAbs(0.75, 0.001));
    REQUIRE_THAT(inputs.offset_ms, WithinAbs(-27.0, 0.001));
    REQUIRE_THAT(inputs.time_right_ms, WithinAbs(911.0, 0.001));
    REQUIRE(inputs.division_right == 10);
    REQUIRE(inputs.routing == Routing::ping_pong);
    REQUIRE_THAT(inputs.tempo_bpm, WithinAbs(137.5, 1.0e-9));
}

TEST_CASE("Character engine bank switches topology through a bounded crossfade",
          "[pulp-delay][characters]") {
    CharacterEngineBank bank;
    bank.prepare(48000.0, Character::tape);
    CharacterEngineConfig config;
    config.time_ms = 1.0f;
    config.right_time_ms = 1.0f;
    config.right_uses_ratio = false;
    bank.apply(config);
    bank.request_character(Character::clean);

    std::array<float, 256> left{};
    std::array<float, 256> right{};
    std::array<float, 256> alternate_left{};
    std::array<float, 256> alternate_right{};
    for (int block = 0; block < 16 && bank.is_crossfading(); ++block) {
        // The processor polls the parameter every callback. Repeating the same
        // request must not restart the incoming engine's priming window.
        bank.request_character(Character::clean);
        bank.process(left.data(), right.data(), static_cast<int>(left.size()),
                     alternate_left.data(), alternate_right.data());
    }

    REQUIRE_FALSE(bank.is_crossfading());
    REQUIRE(bank.active_character() == Character::clean);
    for (float sample : left)
        REQUIRE(std::isfinite(sample));
    for (float sample : right)
        REQUIRE(std::isfinite(sample));
}

TEST_CASE("Character engine bank preserves the latest rapid request",
          "[pulp-delay][characters][automation]") {
    CharacterEngineBank bank;
    bank.prepare(48000.0, Character::tape);
    CharacterEngineConfig config;
    config.time_ms = 1.0f;
    config.right_time_ms = 1.0f;
    config.right_uses_ratio = false;
    bank.apply(config);

    bank.request_character(Character::clean);
    bank.request_character(Character::vintage);
    bank.request_character(Character::bbd);
    REQUIRE(bank.target_character() == Character::bbd);

    std::array<float, 256> left{};
    std::array<float, 256> right{};
    std::array<float, 256> alternate_left{};
    std::array<float, 256> alternate_right{};
    for (int block = 0; block < 24; ++block) {
        bank.process(left.data(), right.data(), static_cast<int>(left.size()),
                     alternate_left.data(), alternate_right.data());
    }
    REQUIRE_FALSE(bank.is_crossfading());
    REQUIRE(bank.active_character() == Character::bbd);
}

TEST_CASE("Rapid alternating character automation cannot restart priming forever",
          "[pulp-delay][characters][automation][bounded]") {
    CharacterEngineBank bank;
    bank.prepare(48000.0, Character::tape);
    CharacterEngineConfig config;
    config.time_ms = 2000.0f;
    config.right_time_ms = 2000.0f;
    config.right_uses_ratio = false;
    bank.apply(config);
    bank.request_character(Character::clean);

    std::array<float, 256> left{};
    std::array<float, 256> right{};
    std::array<float, 256> alternate_left{};
    std::array<float, 256> alternate_right{};
    for (int block = 0; block < 400; ++block) {
        bank.request_character(
            block % 2 == 0 ? Character::vintage : Character::bbd);
        bank.process(left.data(), right.data(), static_cast<int>(left.size()),
                     alternate_left.data(), alternate_right.data());
    }

    // One full 2 s priming window plus the 20 ms fade has elapsed. Alternating
    // requests may replace the pending target, but cannot prevent the already
    // primed transition from becoming audible.
    REQUIRE(bank.active_character() == Character::clean);

    bank.request_character(Character::bbd);
    int settle_blocks = 0;
    while (bank.is_crossfading() && settle_blocks++ < 1024) {
        bank.process(left.data(), right.data(), static_cast<int>(left.size()),
                     alternate_left.data(), alternate_right.data());
    }
    REQUIRE_FALSE(bank.is_crossfading());
    REQUIRE(bank.active_character() == Character::bbd);
}

TEST_CASE("Character request preserves a frozen tail until unfreeze",
          "[pulp-delay][characters][audio][freeze]") {
    CharacterEngineBank bank;
    bank.prepare(48000.0, Character::clean);
    CharacterEngineConfig config;
    config.time_ms = 8.0f;
    config.right_time_ms = 8.0f;
    config.right_uses_ratio = false;
    config.feedback = 0.95f;
    config.crossfeed = 0.0f;
    config.character_amount = 0.0f;
    config.diffusion = 0.0f;
    config.duck = 0.0f;
    config.low_cut_hz = 20.0f;
    config.high_cut_hz = 20000.0f;
    config.mod_depth = 0.0f;
    bank.apply(config);

    std::array<float, 256> left{};
    std::array<float, 256> right{};
    std::array<float, 256> alternate_left{};
    std::array<float, 256> alternate_right{};
    const auto process_block = [&] {
        bank.process(left.data(), right.data(), static_cast<int>(left.size()),
                     alternate_left.data(), alternate_right.data());
    };
    const auto clear_block = [&] {
        left.fill(0.0f);
        right.fill(0.0f);
        alternate_left.fill(0.0f);
        alternate_right.fill(0.0f);
    };
    const auto block_peak = [&] {
        float peak = 0.0f;
        for (std::size_t index = 0; index < left.size(); ++index) {
            peak = std::max(peak, std::abs(left[index]));
            peak = std::max(peak, std::abs(right[index]));
        }
        return peak;
    };

    left[0] = 1.0f;
    right[0] = 1.0f;
    process_block();
    for (int block = 0; block < 3; ++block) {
        clear_block();
        process_block();
    }

    config.freeze = true;
    bank.apply(config);
    float frozen_peak = 0.0f;
    for (int block = 0; block < 6; ++block) {
        clear_block();
        process_block();
        frozen_peak = std::max(frozen_peak, block_peak());
    }
    REQUIRE(frozen_peak > 1.0e-3f);

    bank.request_character(Character::vintage);
    REQUIRE(bank.target_character() == Character::vintage);
    REQUIRE(bank.active_character() == Character::clean);
    REQUIRE_FALSE(bank.is_crossfading());
    float held_peak = 0.0f;
    for (int block = 0; block < 8; ++block) {
        clear_block();
        process_block();
        held_peak = std::max(held_peak, block_peak());
    }
    INFO("frozen peak before request " << frozen_peak
                                       << "; held peak " << held_peak);
    REQUIRE(held_peak > frozen_peak * 0.5f);
    REQUIRE(bank.active_character() == Character::clean);

    config.freeze = false;
    bank.apply(config);
    int transition_blocks = 0;
    while (bank.is_crossfading() && transition_blocks++ < 256) {
        clear_block();
        process_block();
    }
    REQUIRE_FALSE(bank.is_crossfading());
    REQUIRE(bank.active_character() == Character::vintage);
}

TEST_CASE("Freeze defers a target queued during an in-flight crossfade",
          "[pulp-delay][characters][automation][freeze]") {
    CharacterEngineBank bank;
    bank.prepare(48000.0, Character::tape);
    CharacterEngineConfig config;
    config.time_ms = 1.0f;
    config.right_time_ms = 1.0f;
    config.right_uses_ratio = false;
    bank.apply(config);
    bank.request_character(Character::clean);

    std::array<float, 256> left{};
    std::array<float, 256> right{};
    std::array<float, 256> alternate_left{};
    std::array<float, 256> alternate_right{};
    const auto process_block = [&] {
        bank.process(left.data(), right.data(), static_cast<int>(left.size()),
                     alternate_left.data(), alternate_right.data());
    };

    // The minimum priming window is the 20 ms fade length (960 samples).
    for (int block = 0; block < 4; ++block)
        process_block();
    REQUIRE(bank.is_crossfading());

    config.freeze = true;
    bank.apply(config);
    bank.request_character(Character::bbd);
    for (int block = 0; block < 4; ++block)
        process_block();

    REQUIRE_FALSE(bank.is_crossfading());
    REQUIRE(bank.active_character() == Character::clean);
    REQUIRE(bank.target_character() == Character::bbd);

    config.freeze = false;
    bank.apply(config);
    int transition_blocks = 0;
    while (bank.is_crossfading() && transition_blocks++ < 16)
        process_block();
    REQUIRE_FALSE(bank.is_crossfading());
    REQUIRE(bank.active_character() == Character::bbd);
}

TEST_CASE("Character engine bank does not resume paused history after A-B-A switching",
          "[pulp-delay][characters][audio][roundtrip]") {
    CharacterEngineBank bank;
    CharacterEngineBank reference;
    bank.prepare(48000.0, Character::clean);
    reference.prepare(48000.0, Character::clean);
    CharacterEngineConfig config;
    config.time_ms = 8.0f;
    config.right_time_ms = 8.0f;
    config.right_uses_ratio = false;
    config.feedback = 0.95f;
    config.crossfeed = 0.0f;
    config.character_amount = 0.0f;
    config.diffusion = 0.0f;
    config.duck = 0.0f;
    config.low_cut_hz = 20.0f;
    config.high_cut_hz = 20000.0f;
    config.mod_depth = 0.0f;
    bank.apply(config);
    reference.apply(config);

    struct Buffers {
        std::array<float, 256> left{};
        std::array<float, 256> right{};
        std::array<float, 256> alternate_left{};
        std::array<float, 256> alternate_right{};
    };
    Buffers actual;
    Buffers expected;
    const auto process_silence = [](CharacterEngineBank& engine, Buffers& buffers) {
        buffers.left.fill(0.0f);
        buffers.right.fill(0.0f);
        buffers.alternate_left.fill(0.0f);
        buffers.alternate_right.fill(0.0f);
        engine.process(buffers.left.data(), buffers.right.data(),
                       static_cast<int>(buffers.left.size()),
                       buffers.alternate_left.data(),
                       buffers.alternate_right.data());
    };

    actual.left[0] = 1.0f;
    actual.right[0] = 1.0f;
    expected.left[0] = 1.0f;
    expected.right[0] = 1.0f;
    bank.process(actual.left.data(), actual.right.data(),
                 static_cast<int>(actual.left.size()),
                 actual.alternate_left.data(), actual.alternate_right.data());
    reference.process(expected.left.data(), expected.right.data(),
                      static_cast<int>(expected.left.size()),
                      expected.alternate_left.data(), expected.alternate_right.data());
    process_silence(bank, actual);
    process_silence(reference, expected);

    bank.request_character(Character::vintage);
    while (bank.is_crossfading()) {
        process_silence(bank, actual);
        process_silence(reference, expected);
    }
    REQUIRE(bank.active_character() == Character::vintage);
    for (int block = 0; block < 8; ++block) {
        process_silence(bank, actual);
        process_silence(reference, expected);
    }

    bank.request_character(Character::clean);
    constexpr int kFadeSamples = 960;
    int return_frame = 0;
    float reference_peak = 0.0f;
    float residual_peak = 0.0f;
    while (bank.is_crossfading()) {
        process_silence(bank, actual);
        process_silence(reference, expected);
        for (std::size_t i = 0; i < actual.left.size(); ++i) {
            const float blend = std::clamp(
                static_cast<float>(return_frame + static_cast<int>(i))
                    / static_cast<float>(kFadeSamples),
                0.0f, 1.0f);
            reference_peak =
                std::max(reference_peak, std::abs(expected.left[i]));
            residual_peak = std::max(
                residual_peak, std::abs(actual.left[i] - blend * expected.left[i]));
            residual_peak = std::max(
                residual_peak, std::abs(actual.right[i] - blend * expected.right[i]));
        }
        return_frame += static_cast<int>(actual.left.size());
    }

    INFO("continuously decaying reference peak " << reference_peak
                                                  << "; A-B-A residual peak "
                                                  << residual_peak);
    REQUIRE(reference_peak > 1.0e-4f);
    // Priming deliberately feeds the outgoing wet tail into the incoming
    // character, so returning to A is not sample-identical to an A-only
    // reference. It must remain bounded, however; a paused stale history is
    // several times larger than the continuously evolving reference.
    REQUIRE(residual_peak <= reference_peak * 2.5f);
}

TEST_CASE("Character request work stays below the 192 kHz callback reserve",
          "[pulp-delay][characters][realtime]") {
    CharacterEngineBank bank;
    bank.prepare(192000.0, Character::tape);

    // A 16-frame callback at 192 kHz lasts 83.3 us. Character selection gets
    // at most 60% of that interval so the rest of the processor still has a
    // useful deadline reserve. Capacity-sized reset work violates this budget.
    constexpr double kRequestBudgetUs = 50.0;
    constexpr std::size_t kIterations = 64;
    std::array<double, kIterations> elapsed_us{};
    for (std::size_t iteration = 0; iteration < kIterations; ++iteration) {
        bank.reset(); // deliberately outside the timed live-control operation
        const auto begin = std::chrono::steady_clock::now();
        bank.request_character(Character::clean);
        const auto end = std::chrono::steady_clock::now();
        elapsed_us[iteration] =
            std::chrono::duration<double, std::micro>(end - begin).count();
    }
    std::sort(elapsed_us.begin(), elapsed_us.end());
    const double p99_us = elapsed_us[kIterations - 2];
    INFO("character request p99 " << p99_us << " us; budget "
                                  << kRequestBudgetUs << " us");
    REQUIRE(p99_us < kRequestBudgetUs);
}

TEST_CASE("Character engine bank steady processing keeps realtime reserve",
          "[pulp-delay][characters][realtime][performance]") {
    constexpr double kSampleRate = 48000.0;
    constexpr int kBlockSize = 128;
    constexpr std::size_t kBlocksPerTrial = 256;
    constexpr std::size_t kTrials = 9;
    constexpr double kMaxRelativeCost = 2.5;
    constexpr double kMaxCallbackFraction = 0.10;
    constexpr double kCallbackUs =
        static_cast<double>(kBlockSize) / kSampleRate * 1.0e6;

    CharacterEngineConfig config;
    signal::CharacterDelay baseline;
    baseline.set_character(signal::CharacterDelay::Character::tape);
    baseline.set_sample_rate(kSampleRate);
    baseline.set_time_ms(config.time_ms);
    baseline.set_time_offset(config.time_offset);
    baseline.set_feedback(config.feedback);
    baseline.set_crossfeed(config.crossfeed);
    baseline.set_character_amount(config.character_amount);
    baseline.set_diffusion_amount(config.diffusion);
    baseline.set_duck(config.duck);
    baseline.set_loop_low_cut_hz(config.low_cut_hz);
    baseline.set_loop_high_cut_hz(config.high_cut_hz);
    baseline.set_loop_low_cut_resonance(config.low_cut_resonance);
    baseline.set_loop_high_cut_resonance(config.high_cut_resonance);
    baseline.set_mod(config.mod_rate_normalized, config.mod_depth);
    baseline.set_freeze(config.freeze);
    baseline.set_reverse(config.reverse);

    CharacterEngineBank bank;
    bank.prepare(kSampleRate, Character::tape);
    bank.apply(config);

    std::array<float, kBlockSize> baseline_left{};
    std::array<float, kBlockSize> baseline_right{};
    std::array<float, kBlockSize> bank_left{};
    std::array<float, kBlockSize> bank_right{};
    std::array<float, kBlockSize> alternate_left{};
    std::array<float, kBlockSize> alternate_right{};

    // Batch timing amortizes the clock read. Process CPU time deliberately
    // excludes unrelated runner load; wall time makes this gate fail whenever
    // CTest schedules another compile/test beside it.
    const auto measure = [](auto&& process_block) {
        std::array<double, kTrials> elapsed_us{};
        for (std::size_t trial = 0; trial < kTrials; ++trial) {
            const auto begin = std::clock();
            for (std::size_t block = 0; block < kBlocksPerTrial; ++block)
                process_block();
            const auto end = std::clock();
            elapsed_us[trial] =
                static_cast<double>(end - begin) * 1.0e6
                / static_cast<double>(CLOCKS_PER_SEC)
                / static_cast<double>(kBlocksPerTrial);
        }
        std::sort(elapsed_us.begin(), elapsed_us.end());
        return elapsed_us[kTrials / 2];
    };

    for (int block = 0; block < 32; ++block) {
        baseline.process(baseline_left.data(), baseline_right.data(), kBlockSize);
        bank.process(bank_left.data(), bank_right.data(), kBlockSize,
                     alternate_left.data(), alternate_right.data());
    }
    const double baseline_us = measure([&] {
        baseline.process(baseline_left.data(), baseline_right.data(), kBlockSize);
    });
    const double bank_us = measure([&] {
        bank.process(bank_left.data(), bank_right.data(), kBlockSize,
                     alternate_left.data(), alternate_right.data());
    });
    const double relative_cost = bank_us / std::max(baseline_us, 0.001);
    const double callback_fraction = bank_us / kCallbackUs;

    INFO("active-only baseline " << baseline_us << " us/block; two-engine bank "
                                  << bank_us << " us/block; delta "
                                  << (bank_us - baseline_us) << " us; multiplier "
                                  << relative_cost << "x; callback fraction "
                                  << callback_fraction);
    REQUIRE(relative_cost < kMaxRelativeCost);
    REQUIRE(callback_fraction < kMaxCallbackFraction);
}

TEST_CASE("Pulp Delay zero mix is exact dry pass-through", "[pulp-delay][audio]") {
    format::HeadlessHost host(create_pulp_delay);
    host.state().set_value(kMix, 0.0f);
    host.prepare(48000.0, 128);

    audio::Buffer<float> input(2, 128);
    audio::Buffer<float> output(2, 128);
    for (std::size_t i = 0; i < input.num_samples(); ++i) {
        input.channel(0)[i] = static_cast<float>(i) / 127.0f;
        input.channel(1)[i] = -input.channel(0)[i];
    }
    process(host, input, output);

    for (std::size_t i = 0; i < input.num_samples(); ++i) {
        REQUIRE_THAT(output.channel(0)[i], WithinAbs(input.channel(0)[i], 1e-7));
        REQUIRE_THAT(output.channel(1)[i], WithinAbs(input.channel(1)[i], 1e-7));
    }
}

TEST_CASE("Pulp Delay wet path is a real delayed signal", "[pulp-delay][audio]") {
    format::HeadlessHost host(create_pulp_delay);
    host.state().set_value(kCharacter, static_cast<float>(Character::clean));
    host.state().set_value(kTime, 20.0f);
    host.state().set_value(kMix, 100.0f);
    host.state().set_value(kFeedback, 0.0f);
    host.state().set_value(kLowCut, 20.0f);
    host.state().set_value(kHighCut, 20000.0f);
    host.prepare(48000.0, 128);

    audio::Buffer<float> input(2, 128);
    audio::Buffer<float> output(2, 128);
    input.clear();
    input.channel(0)[0] = 1.0f;
    input.channel(1)[0] = 1.0f;
    process(host, input, output);
    const auto early_metrics = test::audio::analyze(output, 48000.0);
    const auto early_silence = test::audio::assert_silent(early_metrics, -80.0);
    INFO(early_silence.message);
    REQUIRE(early_silence.passed);

    double later_peak = 0.0;
    input.clear();
    for (int block = 0; block < 20; ++block) {
        output.clear();
        process(host, input, output);
        const auto metrics = test::audio::analyze(output, 48000.0);
        INFO(test::audio::summarize(metrics));
        REQUIRE(test::audio::assert_no_nan_inf(metrics).passed);
        later_peak = std::max(later_peak, metrics.max_peak());
    }
    REQUIRE(later_peak > 0.01f);
}

TEST_CASE("Pulp Delay mono routing produces matched output channels", "[pulp-delay][routing]") {
    format::HeadlessHost host(create_pulp_delay);
    host.state().set_value(kRouting, static_cast<float>(Routing::mono));
    host.state().set_value(kMix, 0.0f);
    host.prepare(48000.0, 128);

    audio::Buffer<float> input(2, 128);
    audio::Buffer<float> output(2, 128);
    input.clear();
    input.channel(0)[0] = 1.0f;
    process(host, input, output);
    REQUIRE_THAT(output.channel(0)[0], WithinAbs(0.5, 1e-7));
    REQUIRE_THAT(output.channel(1)[0], WithinAbs(0.5, 1e-7));
}

TEST_CASE("Pulp Delay mix automation ramps across the block boundary",
          "[pulp-delay][audio][automation][discontinuity]") {
    format::HeadlessHost host(create_pulp_delay);
    host.state().set_value(kCharacter, static_cast<float>(Character::clean));
    host.state().set_value(kTime, 2000.0f);
    host.state().set_value(kFeedback, 0.0f);
    host.state().set_value(kMix, 0.0f);
    host.prepare(48000.0, 64);

    audio::Buffer<float> input(2, 64);
    audio::Buffer<float> output(2, 64);
    std::fill(input.channel(0).begin(), input.channel(0).end(), 0.75f);
    std::fill(input.channel(1).begin(), input.channel(1).end(), -0.50f);
    process(host, input, output);
    const float previous_left = output.channel(0).back();
    const float previous_right = output.channel(1).back();

    host.state().set_value(kMix, 100.0f);
    process(host, input, output);

    constexpr float kMaxAutomationStep = 0.02f;
    REQUIRE(max_step(output, 0, previous_left) < kMaxAutomationStep);
    REQUIRE(max_step(output, 1, previous_right) < kMaxAutomationStep);
}

TEST_CASE("Pulp Delay routing automation crossfades stereo to mono",
          "[pulp-delay][routing][automation][discontinuity]") {
    format::HeadlessHost host(create_pulp_delay);
    host.state().set_value(kMix, 0.0f);
    host.state().set_value(kRouting, static_cast<float>(Routing::stereo));
    host.prepare(48000.0, 64);

    audio::Buffer<float> input(2, 64);
    audio::Buffer<float> output(2, 64);
    std::fill(input.channel(0).begin(), input.channel(0).end(), 1.0f);
    std::fill(input.channel(1).begin(), input.channel(1).end(), -1.0f);
    process(host, input, output);
    const float previous_left = output.channel(0).back();
    const float previous_right = output.channel(1).back();

    host.state().set_value(kRouting, static_cast<float>(Routing::mono));
    process(host, input, output);

    constexpr float kMaxRoutingStep = 0.02f;
    REQUIRE(max_step(output, 0, previous_left) < kMaxRoutingStep);
    REQUIRE(max_step(output, 1, previous_right) < kMaxRoutingStep);

    for (int block = 0; block < 4; ++block)
        process(host, input, output);
    REQUIRE_THAT(output.channel(0).back(), WithinAbs(0.0, 1.0e-6));
    REQUIRE_THAT(output.channel(1).back(), WithinAbs(0.0, 1.0e-6));
}

TEST_CASE("Pulp Delay safely chunks blocks larger than the prepared maximum",
          "[pulp-delay][audio]") {
    format::HeadlessHost host(create_pulp_delay);
    host.state().set_value(kMix, 0.0f);
    host.prepare(48000.0, 64);

    audio::Buffer<float> input(2, 257);
    audio::Buffer<float> output(2, 257);
    for (std::size_t i = 0; i < input.num_samples(); ++i) {
        input.channel(0)[i] = std::sin(static_cast<float>(i) * 0.1f) * 0.5f;
        input.channel(1)[i] = std::cos(static_cast<float>(i) * 0.07f) * 0.25f;
    }
    process(host, input, output);

    const auto null_result = test::audio::assert_null_near(input, output, -120.0);
    INFO(null_result.message);
    REQUIRE(null_result.passed);
}

TEST_CASE("Pulp Delay state round-trips all 25 parameters", "[pulp-delay][state]") {
    format::HeadlessHost source(create_pulp_delay);
    for (state::ParamID id = kTime; id <= kReverse; ++id) {
        source.state().set_normalized(id, static_cast<float>(id - kTime) /
                                              static_cast<float>(kParameterCount - 1));
    }
    const auto saved = source.save_state();

    format::HeadlessHost restored(create_pulp_delay);
    REQUIRE(restored.load_state(saved));
    for (state::ParamID id = kTime; id <= kReverse; ++id) {
        INFO("parameter id " << id);
        REQUIRE_THAT(restored.state().get_normalized(id),
                     WithinAbs(source.state().get_normalized(id), 1e-6));
    }
}
