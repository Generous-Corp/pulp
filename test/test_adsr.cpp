#include <catch2/catch_test_macros.hpp>
#include <pulp/signal/adsr.hpp>

#include <vector>

using namespace pulp::signal;

TEST_CASE("ADSR runs through stages", "[signal][adsr]") {
    Adsr env;
    env.set_sample_rate(44100.0f);
    env.set_params({0.01f, 0.05f, 0.5f, 0.1f});

    env.note_on();
    REQUIRE(env.stage() == Adsr::Stage::attack);

    // Advance enough samples for attack to complete and sustain to be reached.
    for (int i = 0; i < 44100; ++i) (void) env.next();
    REQUIRE(env.stage() == Adsr::Stage::sustain);

    env.note_off();
    REQUIRE(env.stage() == Adsr::Stage::release);

    // Run release to completion.
    for (int i = 0; i < 44100; ++i) (void) env.next();
    REQUIRE(env.stage() == Adsr::Stage::idle);
    REQUIRE(env.is_active() == false);
}

TEST_CASE("ADSR apply_to_buffer multiplies sample by envelope", "[signal][adsr]") {
    Adsr env;
    env.set_sample_rate(44100.0f);
    env.set_params({0.001f, 0.005f, 0.5f, 0.01f});
    env.note_on();

    // Pre-warm the envelope so it has reached sustain.
    for (int i = 0; i < 1024; ++i) (void) env.next();

    std::vector<float> buffer(256, 1.0f);
    env.apply_to_buffer(buffer.data(), 0, 256);
    // In sustain (level == 0.5), every output should be 0.5.
    for (float s : buffer) {
        REQUIRE(s == 0.5f);
    }
}

TEST_CASE("ADSR apply_to_buffer planar multi-channel", "[signal][adsr]") {
    Adsr env;
    env.set_sample_rate(44100.0f);
    env.set_params({0.001f, 0.005f, 0.5f, 0.01f});
    env.note_on();
    for (int i = 0; i < 1024; ++i) (void) env.next();

    std::vector<float> left(64, 2.0f);
    std::vector<float> right(64, 2.0f);
    float* channels[2] = {left.data(), right.data()};
    env.apply_to_buffer(channels, 2, 0, 64);

    // Both channels see the same per-sample multiplier (0.5 at sustain),
    // so output is 1.0.
    for (int i = 0; i < 64; ++i) {
        REQUIRE(left[i] == 1.0f);
        REQUIRE(right[i] == 1.0f);
    }
}

TEST_CASE("ADSR retrigger does not reset level", "[signal][adsr]") {
    Adsr env;
    env.set_sample_rate(44100.0f);
    env.set_params({0.1f, 0.1f, 0.5f, 0.1f});
    env.note_on();
    for (int i = 0; i < 2000; ++i) (void) env.next();
    const float before = env.next();

    env.note_on(); // retrigger
    const float after = env.next();
    // After retrigger we go back to attack stage; level shouldn't jump to 0.
    REQUIRE(after >= before * 0.5f);
}
