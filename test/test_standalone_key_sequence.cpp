#include <catch2/catch_test_macros.hpp>

#include <pulp/format/detail/standalone_key_driver.hpp>
#include <pulp/format/detail/standalone_key_schedule.hpp>
#include <pulp/format/detail/standalone_key_sequence.hpp>
#include <pulp/format/detail/standalone_environment.hpp>
#include <pulp/format/standalone.hpp>

#include <cstdlib>
#include <string>
#include <vector>

using namespace pulp;
using namespace pulp::format::detail;
using KC = pulp::view::KeyCode;

TEST_CASE("key sequence parses arrow and named keys", "[standalone][keyboard]") {
    std::vector<KeySequenceStep> steps;
    std::string error;
    REQUIRE(parse_key_sequence("down,down,up,return", steps, error));
    REQUIRE(error.empty());
    REQUIRE(steps.size() == 4);
    CHECK(steps[0].key == KC::down);
    CHECK(steps[1].key == KC::down);
    CHECK(steps[2].key == KC::up);
    CHECK(steps[3].key == KC::enter);
    for (const auto& s : steps)
        CHECK(s.modifiers == view::kModNone);
}

TEST_CASE("key sequence parses modifier chords", "[standalone][keyboard]") {
    std::vector<KeySequenceStep> steps;
    std::string error;
    REQUIRE(parse_key_sequence("cmd+a, shift+tab, ctrl+alt+f7", steps, error));
    REQUIRE(steps.size() == 3);

    CHECK(steps[0].key == KC::a);
    CHECK(steps[0].modifiers == view::kModCmd);
    CHECK(steps[0].label == "cmd+a");

    CHECK(steps[1].key == KC::tab);
    CHECK(steps[1].modifiers == view::kModShift);

    CHECK(steps[2].key == KC::f7);
    CHECK(steps[2].modifiers == (view::kModCtrl | view::kModAlt));
}

TEST_CASE("key sequence normalizes case and whitespace", "[standalone][keyboard]") {
    std::vector<KeySequenceStep> steps;
    std::string error;
    REQUIRE(parse_key_sequence("  DOWN , Cmd + A ", steps, error));
    REQUIRE(steps.size() == 2);
    CHECK(steps[0].key == KC::down);
    CHECK(steps[0].label == "down");
    CHECK(steps[1].key == KC::a);
    CHECK(steps[1].modifiers == view::kModCmd);
    // Inner whitespace is stripped so "Cmd + A" and "cmd+a" name one artifact.
    CHECK(steps[1].label == "cmd+a");
}

// Fail-closed is the point: a harness that silently skipped a mistyped key
// would report a green run that pressed fewer keys than it was asked to.
TEST_CASE("key sequence rejects an unknown token outright", "[standalone][keyboard]") {
    std::vector<KeySequenceStep> steps;
    std::string error;

    CHECK_FALSE(parse_key_sequence("down,notakey,down", steps, error));
    CHECK(steps.empty());
    CHECK(error.find("notakey") != std::string::npos);

    CHECK_FALSE(parse_key_sequence("hyper+a", steps, error));
    CHECK(steps.empty());
    CHECK(error.find("hyper") != std::string::npos);

    CHECK_FALSE(parse_key_sequence("f13", steps, error));
    CHECK(steps.empty());
}

TEST_CASE("empty key sequence parses to no steps", "[standalone][keyboard]") {
    std::vector<KeySequenceStep> steps;
    std::string error;
    REQUIRE(parse_key_sequence("", steps, error));
    CHECK(steps.empty());
    CHECK(error.empty());
}

namespace {

struct Observed {
    std::vector<std::string> log;
};

Observed run_schedule(KeySequenceSchedule& schedule, int frames) {
    Observed obs;
    for (int i = 0; i < frames; ++i) {
        const auto tick = schedule.tick();
        switch (tick.action) {
        case KeySequenceSchedule::Action::wait:
            break;
        case KeySequenceSchedule::Action::capture_initial:
            obs.log.push_back("capture-initial@" + std::to_string(i + 1));
            break;
        case KeySequenceSchedule::Action::press:
            obs.log.push_back("press" + std::to_string(tick.index) + "@" +
                              std::to_string(i + 1));
            break;
        case KeySequenceSchedule::Action::capture:
            obs.log.push_back("capture" + std::to_string(tick.index) + "@" +
                              std::to_string(i + 1));
            break;
        case KeySequenceSchedule::Action::finish:
            obs.log.push_back("finish@" + std::to_string(i + 1));
            break;
        }
    }
    return obs;
}

std::vector<KeySequenceStep> two_downs() {
    std::vector<KeySequenceStep> steps;
    std::string error;
    REQUIRE(parse_key_sequence("down,down", steps, error));
    return steps;
}

}  // namespace

// The ordering is what decides whether a captured frame actually shows the
// effect of the press it is named after: every press must be followed by its
// own settle window before the capture, and by another before the next press.
TEST_CASE("key schedule alternates press and capture on the frame grid",
          "[standalone][keyboard]") {
    KeySequenceSchedule schedule(two_downs(), /*frame_delay=*/5);
    const auto obs = run_schedule(schedule, schedule.total_frames() + 5);

    const std::vector<std::string> expected{
        "capture-initial@5",  // the UI before any key
        "press0@10", "capture0@15",
        "press1@20", "capture1@25",
        "finish@30",
    };
    CHECK(obs.log == expected);
    CHECK(schedule.done());
    CHECK(schedule.total_frames() == 30);
}

// finish must land a full slot AFTER the last capture, never in the same
// frame: a caller that closes the window on finish would otherwise close it
// while the capture is still reading pixels out of the surface.
TEST_CASE("key schedule finishes a slot after the last capture",
          "[standalone][keyboard]") {
    KeySequenceSchedule schedule(two_downs(), /*frame_delay=*/1);
    const auto obs = run_schedule(schedule, 10);
    REQUIRE(obs.log.size() >= 2);
    CHECK(obs.log[obs.log.size() - 1] == "finish@6");
    CHECK(obs.log[obs.log.size() - 2] == "capture1@5");
}

TEST_CASE("key schedule with no steps finishes immediately",
          "[standalone][keyboard]") {
    KeySequenceSchedule schedule({}, /*frame_delay=*/3);
    const auto obs = run_schedule(schedule, 12);
    const std::vector<std::string> expected{"finish@3"};
    CHECK(obs.log == expected);
    CHECK(schedule.done());
}

TEST_CASE("key schedule clamps a non-positive frame delay",
          "[standalone][keyboard]") {
    KeySequenceSchedule schedule(two_downs(), /*frame_delay=*/0);
    const auto obs = run_schedule(schedule, 10);
    REQUIRE(obs.log.size() == 6);
    CHECK(obs.log.front() == "capture-initial@1");
}

// A null view handle must be reported, not silently treated as a delivered
// press — an undelivered key that logs success is the exact failure mode that
// makes a harness produce artifacts of a run that never happened.
TEST_CASE("synthetic key delivery refuses a null view", "[standalone][keyboard]") {
    KeySequenceStep step;
    step.key = KC::down;
    CHECK_FALSE(deliver_synthetic_key(nullptr, step));
}

TEST_CASE("synthetic key delivery refuses an unknown key", "[standalone][keyboard]") {
    KeySequenceStep step;  // key defaults to unknown
    int not_a_view = 0;
    CHECK_FALSE(deliver_synthetic_key(&not_a_view, step));
}

TEST_CASE("standalone config carries the key-sequence surface",
          "[standalone][keyboard]") {
    format::StandaloneConfig config;
    CHECK(config.test_key_sequence.empty());
    CHECK(config.test_key_frame_delay > 0);
    CHECK(config.test_key_shot_dir.empty());
}

namespace {

/// Sets an env var for the duration of a scope and restores the prior value.
class ScopedEnv {
public:
    ScopedEnv(const char* name, const char* value) : name_(name) {
        if (const char* prev = std::getenv(name)) {
            had_previous_ = true;
            previous_ = prev;
        }
#if defined(_WIN32)
        _putenv_s(name, value);
#else
        ::setenv(name, value, 1);
#endif
    }
    ~ScopedEnv() {
#if defined(_WIN32)
        _putenv_s(name_.c_str(), had_previous_ ? previous_.c_str() : "");
#else
        if (had_previous_) ::setenv(name_.c_str(), previous_.c_str(), 1);
        else ::unsetenv(name_.c_str());
#endif
    }

private:
    std::string name_;
    std::string previous_;
    bool had_previous_ = false;
};

}  // namespace

TEST_CASE("environment supplies the key-sequence surface", "[standalone][keyboard]") {
    ScopedEnv seq("PULP_TEST_KEY_SEQUENCE", "down,down,return");
    ScopedEnv frames("PULP_TEST_KEY_FRAMES", "12");
    ScopedEnv dir("PULP_TEST_KEY_SHOT_DIR", "/tmp/pulp-keys");

    const auto config = standalone_config_from_environment(format::StandaloneConfig{});
    CHECK(config.test_key_sequence == "down,down,return");
    CHECK(config.test_key_frame_delay == 12);
    CHECK(config.test_key_shot_dir == "/tmp/pulp-keys");
}

// An explicit config value is the caller's decision and must win over the
// ambient environment, matching how every other standalone knob behaves.
TEST_CASE("explicit key-sequence config wins over the environment",
          "[standalone][keyboard]") {
    ScopedEnv seq("PULP_TEST_KEY_SEQUENCE", "escape");
    ScopedEnv dir("PULP_TEST_KEY_SHOT_DIR", "/tmp/from-env");

    format::StandaloneConfig config;
    config.test_key_sequence = "cmd+a";
    config.test_key_shot_dir = "/tmp/from-config";

    const auto resolved = standalone_config_from_environment(config);
    CHECK(resolved.test_key_sequence == "cmd+a");
    CHECK(resolved.test_key_shot_dir == "/tmp/from-config");
}

TEST_CASE("a malformed key frame delay leaves the default intact",
          "[standalone][keyboard]") {
    const format::StandaloneConfig defaults;
    ScopedEnv frames("PULP_TEST_KEY_FRAMES", "nonsense");
    const auto config = standalone_config_from_environment(format::StandaloneConfig{});
    CHECK(config.test_key_frame_delay == defaults.test_key_frame_delay);
}
