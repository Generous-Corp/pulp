#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/instrument_voice_allocator.hpp>
#include <pulp/runtime/scoped_no_alloc.hpp>

#include <array>

using pulp::audio::InstrumentVoiceAllocator;
using pulp::audio::InstrumentVoiceTrigger;
using pulp::audio::VoiceState;
using pulp::audio::VoiceStealPolicy;
using pulp::audio::VoiceTermination;
using pulp::audio::VoiceTerminationReason;
using pulp::audio::kDefaultVoiceTerminationFadeFrames;
using pulp::audio::kInvalidSampleId;

TEST_CASE("InstrumentVoiceAllocator prepares fixed voice storage",
          "[audio][sampler][voices]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE_FALSE(allocator.prepare(0));
    REQUIRE(allocator.max_voices() == 0);

    REQUIRE(allocator.prepare(2));
    REQUIRE(allocator.max_voices() == 2);
    REQUIRE(allocator.active_voice_count() == 0);

    const auto first = allocator.trigger(InstrumentVoiceTrigger{
        .note = 60,
        .sample_id = 10,
    });
    REQUIRE(first.allocated);
    REQUIRE_FALSE(first.stolen);
    REQUIRE(first.voice_index == 0);
    REQUIRE(first.voice_id == 1);
    REQUIRE(allocator.active_voice_count() == 1);
    REQUIRE(allocator.allocated_voice_count() == 1);

    const auto second = allocator.trigger(InstrumentVoiceTrigger{
        .note = 64,
        .sample_id = 11,
    });
    REQUIRE(second.allocated);
    REQUIRE(second.voice_index == 1);
    REQUIRE(second.voice_id == 2);
    REQUIRE(allocator.active_voice_count() == 2);
    REQUIRE(allocator.allocated_voice_count() == 2);
}

TEST_CASE("InstrumentVoiceAllocator rejects invalid triggers",
          "[audio][sampler][voices]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(1));

    REQUIRE_FALSE(allocator.trigger(InstrumentVoiceTrigger{
        .note = -1,
        .sample_id = 10,
    }).allocated);
    REQUIRE_FALSE(allocator.trigger(InstrumentVoiceTrigger{
        .note = 60,
        .sample_id = kInvalidSampleId,
    }).allocated);
    REQUIRE(allocator.active_voice_count() == 0);
}

TEST_CASE("InstrumentVoiceAllocator releases voices by index and note",
          "[audio][sampler][voices]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(3));

    REQUIRE(allocator.trigger(InstrumentVoiceTrigger{.note = 60, .sample_id = 1}).allocated);
    REQUIRE(allocator.trigger(InstrumentVoiceTrigger{.note = 60, .sample_id = 2}).allocated);
    REQUIRE(allocator.trigger(InstrumentVoiceTrigger{.note = 64, .sample_id = 3}).allocated);
    REQUIRE(allocator.active_voice_count() == 3);

    REQUIRE(allocator.release_voice(2));
    REQUIRE_FALSE(allocator.release_voice(2));
    REQUIRE(allocator.active_voice_count() == 2);
    REQUIRE(allocator.allocated_voice_count() == 3);
    REQUIRE(allocator.voices()[2].state == VoiceState::Released);
    REQUIRE(allocator.finish_voice(2));
    REQUIRE(allocator.allocated_voice_count() == 2);

    REQUIRE(allocator.release_note(60, 1) == 1);
    REQUIRE(allocator.active_voice_count() == 1);
    REQUIRE(allocator.allocated_voice_count() == 2);

    REQUIRE(allocator.release_note(60) == 1);
    REQUIRE(allocator.active_voice_count() == 0);
    REQUIRE(allocator.allocated_voice_count() == 2);
    REQUIRE(allocator.finish_voice(0));
    REQUIRE(allocator.finish_voice(1));
    REQUIRE(allocator.allocated_voice_count() == 0);
}

TEST_CASE("InstrumentVoiceAllocator steals the oldest voice when full",
          "[audio][sampler][voices][steal]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(2));
    allocator.set_steal_policy(VoiceStealPolicy::Oldest);

    const auto first = allocator.trigger(InstrumentVoiceTrigger{.note = 60, .sample_id = 1});
    const auto second = allocator.trigger(InstrumentVoiceTrigger{.note = 62, .sample_id = 2});
    REQUIRE(first.voice_index == 0);
    REQUIRE(second.voice_index == 1);

    std::array<VoiceTermination, 1> terminations{};
    const auto third = allocator.trigger(
        InstrumentVoiceTrigger{.note = 64, .sample_id = 3},
        terminations);
    REQUIRE(third.allocated);
    REQUIRE(third.stolen);
    REQUIRE(third.stolen_voice_index == 0);
    REQUIRE(third.stolen_voice_id == first.voice_id);
    REQUIRE(third.termination_count == 1);
    REQUIRE(third.termination_overflow_count == 0);
    REQUIRE(terminations[0].voice_index == 0);
    REQUIRE(terminations[0].voice_id == first.voice_id);
    REQUIRE(terminations[0].reason == VoiceTerminationReason::Stolen);
    REQUIRE(terminations[0].fade_out_frames == kDefaultVoiceTerminationFadeFrames);
    REQUIRE(third.voice_index == 0);
    REQUIRE(allocator.voices()[0].note == 64);
}

TEST_CASE("InstrumentVoiceAllocator prefers stealing inside the same voice group",
          "[audio][sampler][voices][steal]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(3));
    allocator.set_steal_policy(VoiceStealPolicy::PreferSameVoiceGroupOldest);

    const auto group_one_oldest = allocator.trigger(InstrumentVoiceTrigger{
        .note = 60,
        .sample_id = 1,
        .voice_group = 1,
    });
    allocator.trigger(InstrumentVoiceTrigger{
        .note = 62,
        .sample_id = 2,
        .voice_group = 2,
    });
    allocator.trigger(InstrumentVoiceTrigger{
        .note = 64,
        .sample_id = 3,
        .voice_group = 1,
    });

    const auto next_group_one = allocator.trigger(InstrumentVoiceTrigger{
        .note = 65,
        .sample_id = 4,
        .voice_group = 1,
    });
    REQUIRE(next_group_one.allocated);
    REQUIRE(next_group_one.stolen);
    REQUIRE(next_group_one.stolen_voice_id == group_one_oldest.voice_id);
    REQUIRE(next_group_one.voice_index == group_one_oldest.voice_index);
}

TEST_CASE("InstrumentVoiceAllocator applies choke groups before allocation",
          "[audio][sampler][voices][choke]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(4));

    REQUIRE(allocator.trigger(InstrumentVoiceTrigger{
        .note = 42,
        .sample_id = 10,
        .choke_group = 9,
    }).allocated);
    REQUIRE(allocator.trigger(InstrumentVoiceTrigger{
        .note = 44,
        .sample_id = 11,
        .choke_group = 9,
    }).allocated);
    REQUIRE(allocator.active_voice_count() == 1);

    std::array<VoiceTermination, 1> terminations{};
    const auto open_hat = allocator.trigger(
        InstrumentVoiceTrigger{
            .note = 46,
            .sample_id = 12,
            .choke_group = 9,
        },
        terminations);
    REQUIRE(open_hat.allocated);
    REQUIRE_FALSE(open_hat.stolen);
    REQUIRE(open_hat.choked_count == 1);
    REQUIRE(open_hat.termination_count == 1);
    REQUIRE(open_hat.termination_overflow_count == 0);
    REQUIRE(terminations[0].reason == VoiceTerminationReason::Choked);
    REQUIRE(terminations[0].fade_out_frames == kDefaultVoiceTerminationFadeFrames);
    REQUIRE(allocator.active_voice_count() == 1);
    REQUIRE(allocator.voices()[open_hat.voice_index].note == 46);
}

TEST_CASE("InstrumentVoiceAllocator reports configured termination fade length",
          "[audio][sampler][voices][steal]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(1));
    allocator.set_termination_fade_frames(32);

    const auto first = allocator.trigger(InstrumentVoiceTrigger{.note = 60, .sample_id = 1});
    REQUIRE(first.allocated);

    std::array<VoiceTermination, 1> terminations{};
    const auto second = allocator.trigger(
        InstrumentVoiceTrigger{.note = 62, .sample_id = 2},
        terminations);
    REQUIRE(second.allocated);
    REQUIRE(second.stolen);
    REQUIRE(second.termination_count == 1);
    REQUIRE(terminations[0].voice_id == first.voice_id);
    REQUIRE(terminations[0].fade_out_frames == 32);
}

TEST_CASE("InstrumentVoiceAllocator reports termination overflow without allocation",
          "[audio][sampler][voices][choke]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(3));

    REQUIRE(allocator.trigger(InstrumentVoiceTrigger{
        .note = 42,
        .sample_id = 10,
        .choke_group = 9,
    }).allocated);
    const auto result = allocator.trigger(InstrumentVoiceTrigger{
        .note = 46,
        .sample_id = 12,
        .choke_group = 9,
    });
    REQUIRE(result.allocated);
    REQUIRE(result.choked_count == 1);
    REQUIRE(result.termination_count == 1);
    REQUIRE(result.termination_overflow_count == 1);
}

TEST_CASE("InstrumentVoiceAllocator hot trigger and release do not allocate",
          "[audio][sampler][voices][rt]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(2));

    bool released = false;
    {
        pulp::runtime::ScopedNoAlloc no_alloc;
        REQUIRE(allocator.trigger(InstrumentVoiceTrigger{.note = 60, .sample_id = 1}).allocated);
        REQUIRE(allocator.trigger(InstrumentVoiceTrigger{.note = 62, .sample_id = 2}).allocated);
        REQUIRE(allocator.trigger(InstrumentVoiceTrigger{.note = 64, .sample_id = 3}).allocated);
        released = allocator.release_voice(0);
        REQUIRE(allocator.finish_voice(0));
    }

    REQUIRE(released);
}
