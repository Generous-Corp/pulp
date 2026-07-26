// Character-delay bake-layer contract.
//
// The signal suite proves the delay engine. This file proves what the catalog
// adds: six stable registrations, ten live baked parameters reaching the audio
// through the production injection path, stereo coupling, and an allocation-
// free process path.

#include <catch2/catch_test_macros.hpp>

#include "harness/baked_node_fixture.hpp"
#include "harness/rt_allocation_probe.hpp"

#include <pulp/host/forge_character_delay_catalog.hpp>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

using namespace pulp::host;
namespace catalog = pulp::host::character_delay;
using pulp::test::immediate;

namespace {

constexpr double kSr = 48000.0;
constexpr int kFrames = 256;
constexpr int kCaptureBlocks = 128;
using Fixture = pulp::test::BakedNodeFixture<2>;

struct Realization {
    catalog::Character character;
    catalog::TapeTier tier;
};

constexpr std::array<Realization, 6> kRealizations = {{
    {catalog::Character::clean, catalog::TapeTier::standard},
    {catalog::Character::vintage_digital, catalog::TapeTier::standard},
    {catalog::Character::tape, catalog::TapeTier::standard},
    {catalog::Character::tape, catalog::TapeTier::physical},
    {catalog::Character::bbd, catalog::TapeTier::standard},
    {catalog::Character::diffusion, catalog::TapeTier::standard},
}};

std::vector<float> noise_block(bool invert = false) {
    std::vector<float> out(static_cast<std::size_t>(kFrames));
    std::uint32_t state = 0x51A7E123u;
    for (float& value : out) {
        state ^= state << 13;
        state ^= state >> 17;
        state ^= state << 5;
        const float sample =
            static_cast<float>((static_cast<double>(state >> 8) / 8388608.0 - 1.0) * 0.35);
        value = invert ? -sample : sample;
    }
    return out;
}

std::vector<float> silence() {
    return std::vector<float>(static_cast<std::size_t>(kFrames), 0.0f);
}

void baseline(ParamInjector& injector) {
    REQUIRE(injector.inject(immediate(catalog::kTimeMs, 20.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(catalog::kTimeOffset, 1.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(catalog::kFeedback, 0.45f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(catalog::kCrossfeed, 0.25f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(catalog::kCharacter, 0.65f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(catalog::kModRate, 0.4f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(catalog::kModDepth, 0.6f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(catalog::kDuck, 0.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(catalog::kFreeze, 0.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(catalog::kReverse, 0.0f)) == InjectStatus::Ok);
}

std::vector<float> render_tape_at(pulp::state::ParamID id, float value,
                                  bool force_mod_depth = false) {
    Fixture fixture(catalog::make_character_delay_node(catalog::Character::tape), kSr, kFrames);
    auto injector = fixture.claim_injector();
    baseline(injector);
    if (force_mod_depth)
        REQUIRE(injector.inject(immediate(catalog::kModDepth, 1.0f)) == InjectStatus::Ok);
    REQUIRE(injector.inject(immediate(id, value)) == InjectStatus::Ok);

    const auto left = noise_block();
    const auto right = noise_block(true);
    std::vector<float> captured;
    captured.reserve(static_cast<std::size_t>(2 * kCaptureBlocks * kFrames));
    for (int block = 0; block < kCaptureBlocks; ++block) {
        const auto output = fixture.render({left, right});
        captured.insert(captured.end(), output[0].begin(), output[0].end());
        captured.insert(captured.end(), output[1].begin(), output[1].end());
    }
    return captured;
}

double maximum_difference(const std::vector<float>& a, const std::vector<float>& b) {
    REQUIRE(a.size() == b.size());
    double difference = 0.0;
    for (std::size_t i = 0; i < a.size(); ++i)
        difference = std::max(difference,
                              std::abs(static_cast<double>(a[i]) - static_cast<double>(b[i])));
    return difference;
}

}  // namespace

TEST_CASE("character delay catalog registers six distinct baked realizations",
          "[character-delay][catalog][baked]") {
    std::vector<std::string> ids;
    for (const auto realization : kRealizations) {
        const auto type =
            catalog::make_character_delay_node(realization.character, realization.tier);
        REQUIRE(type.type_id == catalog::character_delay_type_id(realization.character,
                                                                  realization.tier));
        REQUIRE(type.lowerable);
        REQUIRE(type.num_input_ports == 2);
        REQUIRE(type.num_output_ports == 2);
        REQUIRE(type.baked_params.size() == 10);
        REQUIRE(static_cast<bool>(type.process_instance_baked_param));
        ids.emplace_back(type.type_id);

        Fixture fixture(type, kSr, kFrames);
        auto impulse = silence();
        impulse[0] = 1.0f;
        bool heard = false;
        for (int block = 0; block < 96; ++block) {
            const auto output = fixture.render({block == 0 ? impulse : silence(), silence()});
            for (const auto& channel : output)
                for (float value : channel) {
                    REQUIRE(std::isfinite(value));
                    heard = heard || std::abs(value) > 1e-7f;
                }
        }
        REQUIRE(heard);
    }
    std::sort(ids.begin(), ids.end());
    REQUIRE(std::adjacent_find(ids.begin(), ids.end()) == ids.end());
}

TEST_CASE("every character-delay baked parameter reaches the running audio",
          "[character-delay][catalog][baked][param-injection]") {
    struct Probe {
        pulp::state::ParamID id;
        float low;
        float high;
        bool force_mod_depth;
    };
    const std::array<Probe, 10> probes = {{
        {catalog::kTimeMs, 8.0f, 45.0f, false},
        {catalog::kTimeOffset, 0.6f, 1.4f, false},
        {catalog::kFeedback, 0.05f, 0.95f, false},
        {catalog::kCrossfeed, 0.0f, 1.0f, false},
        {catalog::kCharacter, 0.0f, 1.0f, false},
        {catalog::kModRate, 0.05f, 0.95f, true},
        {catalog::kModDepth, 0.0f, 1.0f, false},
        {catalog::kDuck, 0.0f, 1.0f, false},
        {catalog::kFreeze, 0.0f, 1.0f, false},
        {catalog::kReverse, 0.0f, 1.0f, false},
    }};

    for (const auto& probe : probes) {
        const auto low = render_tape_at(probe.id, probe.low, probe.force_mod_depth);
        const auto high = render_tape_at(probe.id, probe.high, probe.force_mod_depth);
        INFO("parameter id " << probe.id);
        for (float value : low) REQUIRE(std::isfinite(value));
        for (float value : high) REQUIRE(std::isfinite(value));
        REQUIRE(maximum_difference(low, high) > 1e-6);
    }
}

TEST_CASE("character-delay baked processing and live injection allocate nothing",
          "[character-delay][catalog][baked][rt-safety]") {
    Fixture fixture(catalog::make_character_delay_node(catalog::Character::tape), kSr, kFrames);
    auto injector = fixture.claim_injector();
    baseline(injector);
    pulp::test::ReusableRenderer<2> renderer(fixture, {noise_block(), noise_block(true)});
    renderer.render();

    pulp::test::RtAllocationProbe probe;
    for (int block = 0; block < 32; ++block) {
        REQUIRE(injector.inject(immediate(catalog::kTimeMs, 5.0f + block)) == InjectStatus::Ok);
        REQUIRE(injector.inject(immediate(catalog::kFeedback, 0.02f * block)) ==
                InjectStatus::Ok);
        REQUIRE(injector.inject(immediate(catalog::kModDepth, 0.03f * block)) ==
                InjectStatus::Ok);
        REQUIRE(injector.inject(immediate(catalog::kReverse,
                                          static_cast<float>(block & 1))) == InjectStatus::Ok);
        renderer.render();
    }
    REQUIRE(probe.allocation_count() == 0);
}
