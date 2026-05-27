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

// Regression: #2970 / Codex comment 3305855151 — the CoreAudio render
// callback used to set `wg_joined_` to true UNCONDITIONALLY, even if
// `join_from_audio_thread()` returned false (a transient failure on the
// first render call). After that, every later render skipped the join,
// permanently leaving the audio thread without workgroup membership /
// RT-priority fallback for the rest of the session.
//
// The fix at coreaudio_device.mm:313 uses the join return value to gate
// the flag flip. We mirror the call-site pattern here so future refactors
// of the workgroup contract can't silently regress the device side:
// AudioWorkgroup::join_from_audio_thread() must always return the actual
// success/failure of the join attempt (NOT "true if attempted"), and
// is_joined() must stay false until a join actually succeeds.
TEST_CASE("AudioWorkgroup join_from_audio_thread reports actual success/failure",
          "[audio][workgroup][issue-2970]") {
    AudioWorkgroup wg;

    // Contract 1: a fresh workgroup is not joined.
    REQUIRE_FALSE(wg.is_joined());

    // Contract 2: the return value of join_from_audio_thread() must equal
    // the resulting is_joined() state. The CoreAudio device callback relies
    // on this to decide whether to flip wg_joined_ — if the return drifted
    // out of sync with is_joined(), the device-level retry loop would
    // either spin forever or, like the bug, stop retrying prematurely.
    bool result = wg.join_from_audio_thread();
    REQUIRE(result == wg.is_joined());

    if (result) {
        // Contract 3 (on success): a second join is idempotent.
        REQUIRE(wg.join_from_audio_thread());
        REQUIRE(wg.is_joined());
        wg.leave();
        REQUIRE_FALSE(wg.is_joined());
        // Contract 4: after leave, a subsequent join attempt is allowed.
        bool rejoin_result = wg.join_from_audio_thread();
        REQUIRE(rejoin_result == wg.is_joined());
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

TEST_CASE("AudioWorkgroup non-Apple fallback join can rejoin after leave",
          "[audio][workgroup][issue-640]") {
    AudioWorkgroup wg;
    REQUIRE(wg.join_from_audio_thread());
    REQUIRE(wg.is_joined());

    wg.leave();
    REQUIRE_FALSE(wg.is_joined());

    REQUIRE(wg.join_from_audio_thread());
    REQUIRE(wg.is_joined());

    wg.leave();
    REQUIRE_FALSE(wg.is_joined());
}

TEST_CASE("AudioWorkgroup non-Apple fallback instances track state independently",
          "[audio][workgroup][issue-640]") {
    AudioWorkgroup first;
    AudioWorkgroup second;

    REQUIRE(first.join_from_audio_thread());
    REQUIRE(first.is_joined());
    REQUIRE_FALSE(second.is_joined());

    REQUIRE(second.join_from_audio_thread());
    REQUIRE(second.is_joined());

    first.leave();
    REQUIRE_FALSE(first.is_joined());
    REQUIRE(second.is_joined());

    second.leave();
    REQUIRE_FALSE(second.is_joined());
}

TEST_CASE("AudioWorkgroup non-Apple priority helpers report fallback success",
          "[audio][workgroup][issue-640]") {
    REQUIRE(AudioWorkgroup::set_high_priority());
    REQUIRE(AudioWorkgroup::set_realtime_priority());
}
#else
TEST_CASE("AudioWorkgroup Apple null workgroup uses safe fallback lifecycle",
          "[audio][workgroup][issue-640]") {
    AudioWorkgroup wg;
    wg.set_workgroup(nullptr);
    wg.leave();
    REQUIRE_FALSE(wg.is_joined());

    bool joined = wg.join_from_audio_thread();
    if (joined) {
        REQUIRE(wg.is_joined());
        wg.leave();
    }
    REQUIRE_FALSE(wg.is_joined());
}
#endif
