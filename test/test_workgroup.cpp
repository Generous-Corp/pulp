#include <catch2/catch_test_macros.hpp>
#include <pulp/audio/workgroup.hpp>

using namespace pulp::audio;

TEST_CASE("AudioWorkgroup default state", "[audio][workgroup]") {
    AudioWorkgroup wg;
    REQUIRE_FALSE(wg.is_joined());
}

TEST_CASE("AudioWorkgroup set_realtime_priority does not crash", "[audio][workgroup]") {
    // May or may not succeed depending on permissions, but should not crash
    bool result = AudioWorkgroup::set_realtime_priority();
    // We don't assert success because it may require root/entitlements
    (void)result;
}

TEST_CASE("AudioWorkgroup set_high_priority does not crash", "[audio][workgroup]") {
    bool result = AudioWorkgroup::set_high_priority();
    (void)result;
}

TEST_CASE("AudioWorkgroup leave without join is safe", "[audio][workgroup]") {
    AudioWorkgroup wg;
    wg.leave(); // should be safe even if never joined
    REQUIRE_FALSE(wg.is_joined());
}

TEST_CASE("AudioWorkgroup join without workgroup uses fallback", "[audio][workgroup]") {
    AudioWorkgroup wg;
    // No workgroup set — should attempt fallback priority
    bool joined = wg.join_from_audio_thread();
    // Result depends on platform permissions
    if (joined) {
        REQUIRE(wg.is_joined());
        wg.leave();
        REQUIRE_FALSE(wg.is_joined());
    }
}

#if !defined(__APPLE__)
TEST_CASE("AudioWorkgroup non-Apple fallback join is idempotent",
          "[audio][workgroup][issue-640]") {
    AudioWorkgroup wg;
    REQUIRE(wg.join_from_audio_thread());
    REQUIRE(wg.is_joined());
    REQUIRE(wg.join_from_audio_thread());
    REQUIRE(wg.is_joined());

    wg.leave();
    REQUIRE_FALSE(wg.is_joined());
    wg.leave();
    REQUIRE_FALSE(wg.is_joined());
}
#endif
