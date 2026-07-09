// The loopback doctor's arithmetic, without an audio interface.
//
// The tool itself cannot be tested here — it opens real hardware and drives real
// voltage. What it can be held to is that every decision it makes about the
// samples it captured is a pure function, tested against vectors that encode the
// failure modes we actually expect: a chain that inverts, a permissions denial
// that looks like silence, and a probe level that must never run away.

#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_floating_point.hpp>

#include <brew/rig.hpp>

#include <vector>

using namespace pulp::examples::brew;
using Catch::Matchers::WithinAbs;

TEST_CASE("channel_stats separates DC from magnitude", "[brew][rig]") {
    const std::vector<float> dc(64, 0.25f);
    const auto s = channel_stats(dc.data(), dc.size());
    REQUIRE_THAT(s.mean, WithinAbs(0.25, 1e-9));
    REQUIRE(s.peak == 0.25f);

    // A symmetric AC signal has no DC component but plenty of peak. A DC probe
    // that reported peak would call this channel "responding" when it is not.
    const std::vector<float> ac = {1.0f, -1.0f, 1.0f, -1.0f};
    const auto a = channel_stats(ac.data(), ac.size());
    REQUIRE_THAT(a.mean, WithinAbs(0.0, 1e-9));
    REQUIRE(a.peak == 1.0f);

    REQUIRE(channel_stats(nullptr, 0).peak == 0.0f);
}

TEST_CASE("responding_channels finds the wired channel and its polarity",
          "[brew][rig]") {
    std::vector<ChannelStats> ch(4);
    ch[0] = {0.0002, 0.001f};   // noise floor
    ch[1] = {0.2490, 0.250f};   // straight wire
    ch[2] = {-0.2510, 0.252f};  // inverted
    ch[3] = {0.0, 0.0f};

    const auto hits = responding_channels(ch, 0.25);
    REQUIRE(hits.size() == 2);
    REQUIRE(hits[0].input_channel == 1);
    REQUIRE_THAT(hits[0].gain, WithinAbs(0.996, 1e-3));
    REQUIRE(hits[1].input_channel == 2);
    // The sign is the whole point: this is what Invert exists to correct.
    REQUIRE(hits[1].gain < 0.0);
}

// The threshold is in gain units so it means the same thing at every drive
// level. A threshold in sample units would silently stop detecting anything as
// the probe level dropped.
TEST_CASE("responding_channels scales its threshold with the drive level",
          "[brew][rig]") {
    const std::vector<ChannelStats> ch = {{0.02, 0.02f}};  // 20% of a 0.1 drive

    REQUIRE(responding_channels(ch, 0.1).size() == 1);   // 0.02 > 0.1 * 0.1
    REQUIRE(responding_channels(ch, 0.5).empty());       // 0.02 < 0.1 * 0.5

    // A zero drive level can never be divided by; report nothing, don't divide.
    REQUIRE(responding_channels(ch, 0.0).empty());
}

// The lesson this encodes: on macOS a process denied microphone access gets
// frames of literal 0.0f and no error. A real converter input always has a noise
// floor, so exact zeros mean permissions, not cables. Reporting them as the same
// thing sends you hunting for a patch problem that does not exist.
TEST_CASE("all_exactly_zero distinguishes a denial from a quiet input",
          "[brew][rig]") {
    REQUIRE(all_exactly_zero({{0.0, 0.0f}, {0.0, 0.0f}}));

    // One sample of noise anywhere means the converter is really talking to us.
    REQUIRE_FALSE(all_exactly_zero({{0.0, 0.0f}, {0.0, 1e-7f}}));

    // No channels at all is not a denial — it is a device with no inputs.
    REQUIRE_FALSE(all_exactly_zero({}));
}

TEST_CASE("first_crossing locates the returning edge", "[brew][rig]") {
    std::vector<float> buf(100, 0.0f);
    buf[42] = 0.4f;
    REQUIRE(first_crossing(buf.data(), buf.size(), 0.2f) == 42);

    // Polarity-independent: an inverted chain returns a negative impulse, and a
    // latency measurement that missed it would report no signal at all.
    buf[42] = -0.4f;
    REQUIRE(first_crossing(buf.data(), buf.size(), 0.2f) == 42);

    REQUIRE_FALSE(first_crossing(buf.data(), buf.size(), 0.9f).has_value());
    REQUIRE_FALSE(first_crossing(nullptr, 0, 0.1f).has_value());
}

// This tool probes an unknown chain, so its ceiling is lower than a plug-in's.
// A plug-in emits what the user asked for; a probe must never surprise a module.
TEST_CASE("probe level cannot run away", "[brew][rig][safety]") {
    REQUIRE(clamp_probe_level(1.0f) == kMaxProbeLevel);
    REQUIRE(clamp_probe_level(-9.0f) == -kMaxProbeLevel);
    REQUIRE(clamp_probe_level(0.25f) == 0.25f);
    REQUIRE(kMaxProbeLevel < 1.0f);
}
