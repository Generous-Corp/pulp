#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/unison_voice_stack.hpp>
#include "harness/rt_allocation_probe.hpp"

#include <algorithm>
#include <array>

using namespace pulp::audio;

TEST_CASE("Unison voice stacks allocate and release complete logical notes",
          "[audio][unison-stack]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(8));
    UnisonVoiceStackManager<> manager;
    REQUIRE(manager.prepare(allocator));
    std::array<UnisonVoiceTermination, 8> terms{};
    const auto result = manager.trigger({.source_id = 10, .voice_count = 4,
                                         .note = 60, .sample_id = 1}, terms);
    REQUIRE(result.allocated);
    REQUIRE(result.voice_count == 4);
    REQUIRE(manager.children(10).size() == 4);
    REQUIRE(allocator.active_voice_count() == 4);
    REQUIRE(manager.release(10));
    REQUIRE(allocator.active_voice_count() == 0);
    REQUIRE(allocator.allocated_voice_count() == 4);
}

TEST_CASE("Same-pitch unison stacks release by logical source",
          "[audio][unison-stack][ownership][polyphony]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(4));
    UnisonVoiceStackManager<> manager;
    REQUIRE(manager.prepare(allocator));
    std::array<UnisonVoiceTermination, 4> terminations{};

    REQUIRE(manager.trigger({.source_id = 101, .voice_count = 2, .note = 60,
                             .sample_id = 1}, terminations).allocated);
    REQUIRE(manager.trigger({.source_id = 202, .voice_count = 2, .note = 60,
                             .sample_id = 1}, terminations).allocated);
    REQUIRE(allocator.active_voice_count() == 4);
    REQUIRE(allocator.allocated_voice_count() == 4);

    std::array<UnisonVoiceChild, 2> first{};
    std::array<UnisonVoiceChild, 2> peer{};
    const auto first_view = manager.children(101);
    const auto peer_view = manager.children(202);
    REQUIRE(first_view.size() == first.size());
    REQUIRE(peer_view.size() == peer.size());
    std::copy(first_view.begin(), first_view.end(), first.begin());
    std::copy(peer_view.begin(), peer_view.end(), peer.begin());

    REQUIRE(manager.release(101));
    for (const auto& child : first) {
        const auto& voice = allocator.voices()[child.voice_index];
        REQUIRE(voice.state == VoiceState::Released);
        REQUIRE(voice.voice_id == child.voice_id);
        REQUIRE(voice.note == 60);
        REQUIRE(voice.sample_id == 1);
    }
    for (const auto& child : peer) {
        const auto& voice = allocator.voices()[child.voice_index];
        REQUIRE(voice.state == VoiceState::Active);
        REQUIRE(voice.voice_id == child.voice_id);
        REQUIRE(voice.note == 60);
        REQUIRE(voice.sample_id == 1);
    }
    REQUIRE(allocator.active_voice_count() == 2);
    REQUIRE(allocator.allocated_voice_count() == 4);

    for (const auto& child : first)
        REQUIRE(manager.finish(child.voice_index, child.voice_id, child.generation));
    REQUIRE(manager.children(101).empty());
    REQUIRE(manager.children(202).size() == peer.size());
    REQUIRE(allocator.active_voice_count() == 2);
    REQUIRE(allocator.allocated_voice_count() == 2);
    for (const auto& child : peer) {
        const auto& voice = allocator.voices()[child.voice_index];
        REQUIRE(voice.state == VoiceState::Active);
        REQUIRE(voice.voice_id == child.voice_id);
    }

    REQUIRE(manager.release(202));
    for (const auto& child : peer)
        REQUIRE(manager.finish(child.voice_index, child.voice_id, child.generation));
    REQUIRE(manager.children(202).empty());
    REQUIRE(allocator.active_voice_count() == 0);
    REQUIRE(allocator.allocated_voice_count() == 0);
}

TEST_CASE("Unison voice stack stealing removes whole oldest stack and never itself",
          "[audio][unison-stack][steal]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(6));
    UnisonVoiceStackManager<> manager;
    REQUIRE(manager.prepare(allocator));
    std::array<UnisonVoiceTermination, 8> terms{};
    REQUIRE(manager.trigger({.source_id = 1, .voice_count = 3, .note = 60,
                             .sample_id = 1, .voice_group = 9}, terms).allocated);
    REQUIRE(manager.trigger({.source_id = 2, .voice_count = 3, .note = 64,
                             .sample_id = 2, .voice_group = 9}, terms).allocated);
    const auto third = manager.trigger({.source_id = 3, .voice_count = 4,
                                        .note = 67, .sample_id = 3,
                                        .voice_group = 9}, terms);
    REQUIRE(third.allocated);
    REQUIRE(third.termination_count == 6);
    REQUIRE(manager.children(1).empty());
    REQUIRE(manager.children(2).empty());
    REQUIRE(manager.children(3).size() == 4);
    REQUIRE(allocator.allocated_voice_count() == 4);
}

TEST_CASE("Unison voice stack stealing selects only the complete oldest stack",
          "[audio][unison-stack][steal]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(6));
    UnisonVoiceStackManager<> manager;
    REQUIRE(manager.prepare(allocator));
    std::array<UnisonVoiceTermination, 6> terms{};
    for (std::uint64_t source = 1; source <= 3; ++source)
        REQUIRE(manager
                    .trigger({.source_id = source,
                              .voice_count = 2,
                              .note = static_cast<int>(59 + source),
                              .sample_id = static_cast<std::uint32_t>(source)},
                             terms)
                    .allocated);

    allocator.set_termination_fade_frames(37);
    const auto oldest_view = manager.children(1);
    std::array<UnisonVoiceChild, 2> oldest{};
    std::copy(oldest_view.begin(), oldest_view.end(), oldest.begin());
    const auto replacement =
        manager.trigger({.source_id = 4, .voice_count = 2, .note = 64, .sample_id = 4}, terms);
    REQUIRE(replacement.allocated);
    REQUIRE(replacement.termination_count == oldest.size());
    REQUIRE(manager.children(1).empty());
    REQUIRE(manager.children(2).size() == 2);
    REQUIRE(manager.children(3).size() == 2);
    REQUIRE(manager.children(4).size() == 2);
    for (std::size_t i = 0; i < oldest.size(); ++i) {
        REQUIRE(terms[i].voice.voice_index == oldest[i].voice_index);
        REQUIRE(terms[i].voice.voice_id == oldest[i].voice_id);
        REQUIRE(terms[i].voice.reason == VoiceTerminationReason::Stolen);
        REQUIRE(terms[i].voice.fade_out_frames == 37);
        REQUIRE(terms[i].generation == oldest[i].generation);
    }
}

TEST_CASE("Unison voice stack trigger preflights termination capacity atomically",
          "[audio][unison-stack]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(4));
    UnisonVoiceStackManager<> manager;
    REQUIRE(manager.prepare(allocator));
    std::array<UnisonVoiceTermination, 4> enough{};
    REQUIRE(manager.trigger({.source_id = 1, .voice_count = 4, .note = 60,
                             .sample_id = 1}, enough).allocated);
    std::array<UnisonVoiceTermination, 3> too_small{};
    REQUIRE_FALSE(manager.trigger({.source_id = 2, .voice_count = 4, .note = 62,
                                   .sample_id = 2}, too_small).allocated);
    REQUIRE(manager.children(1).size() == 4);
    REQUIRE(manager.children(2).empty());
}

TEST_CASE("Unison voice stack rejects invalid triggers without changing ownership",
          "[audio][unison-stack]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(4));
    UnisonVoiceStackManager<> manager;
    REQUIRE(manager.prepare(allocator));
    std::array<UnisonVoiceTermination, 4> terms{};
    REQUIRE(manager.trigger({.source_id = 1, .voice_count = 2, .note = 60, .sample_id = 1}, terms)
                .allocated);
    const auto allocated_before = allocator.allocated_voice_count();
    REQUIRE_FALSE(
        manager.trigger({.source_id = 0, .voice_count = 1, .note = 60, .sample_id = 2}, terms)
            .allocated);
    REQUIRE_FALSE(
        manager.trigger({.source_id = 2, .voice_count = 0, .note = 60, .sample_id = 2}, terms)
            .allocated);
    REQUIRE_FALSE(
        manager.trigger({.source_id = 2, .voice_count = 1, .note = 128, .sample_id = 2}, terms)
            .allocated);
    REQUIRE_FALSE(
        manager
            .trigger({.source_id = 2, .voice_count = 1, .note = 60, .sample_id = kInvalidSampleId},
                     terms)
            .allocated);
    REQUIRE(manager.children(1).size() == 2);
    REQUIRE(allocator.allocated_voice_count() == allocated_before);
}

TEST_CASE("Unison voice stacks choke complete matching groups",
          "[audio][unison-stack][choke]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(8));
    UnisonVoiceStackManager<> manager;
    REQUIRE(manager.prepare(allocator));
    std::array<UnisonVoiceTermination, 8> terms{};
    REQUIRE(manager.trigger({.source_id = 1, .voice_count = 3, .note = 60,
                             .sample_id = 1, .choke_group = 7}, terms).allocated);
    const auto next = manager.trigger({.source_id = 2, .voice_count = 2, .note = 61,
                                       .sample_id = 2, .choke_group = 7}, terms);
    REQUIRE(next.allocated);
    REQUIRE(next.termination_count == 3);
    REQUIRE(terms[0].voice.reason == VoiceTerminationReason::Choked);
    REQUIRE(manager.children(1).empty());
}

TEST_CASE("Unison voice stack rejects stale finish tails by exact voice id",
          "[audio][unison-stack]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(2));
    UnisonVoiceStackManager<> manager;
    REQUIRE(manager.prepare(allocator));
    std::array<UnisonVoiceTermination, 2> terms{};
    REQUIRE(manager.trigger({.source_id = 1, .voice_count = 2, .note = 60,
                             .sample_id = 1}, terms).allocated);
    const auto old = manager.children(1)[0];
    REQUIRE_FALSE(manager.finish(old.voice_index, old.voice_id, old.generation));
    REQUIRE(manager.release(1));
    REQUIRE(manager.trigger({.source_id = 2, .voice_count = 2, .note = 64,
                             .sample_id = 2}, terms).allocated);
    REQUIRE_FALSE(manager.finish(old.voice_index, old.voice_id, old.generation));
    const auto current = manager.children(2)[0];
    REQUIRE(manager.release(2));
    REQUIRE(manager.finish(current.voice_index, current.voice_id, current.generation));
    REQUIRE(manager.children(2).size() == 1);
}

TEST_CASE("Unison voice stack panic termination is bounded and reprepare is safe",
          "[audio][unison-stack][termination]") {
    InstrumentVoiceAllocator first, second;
    REQUIRE(first.prepare(4));
    REQUIRE(second.prepare(4));
    UnisonVoiceStackManager<> manager;
    REQUIRE(manager.prepare(first));
    std::array<UnisonVoiceTermination, 4> terms{};
    REQUIRE(manager.trigger({.source_id = 1, .voice_count = 4, .note = 60,
                             .sample_id = 1}, terms).allocated);
    const auto pre_reset_child = manager.children(1)[0];
    REQUIRE_FALSE(manager.prepare(second));
    const auto before = manager.children(1);
    std::array<UnisonVoiceTermination, 3> too_small{};
    REQUIRE_FALSE(manager.terminate_all(too_small).terminated);
    REQUIRE(manager.children(1).size() == before.size());
    const auto panic = manager.terminate_all(terms);
    REQUIRE(panic.terminated);
    REQUIRE(panic.termination_count == 4);
    REQUIRE(first.allocated_voice_count() == 0);
    REQUIRE_FALSE(manager.prepare(second));
    REQUIRE(manager.reset());
    REQUIRE(manager.prepare(second));
    REQUIRE(manager.trigger({.source_id = 2, .voice_count = 1, .note = 62,
                             .sample_id = 2}, terms).allocated);
    const auto post_reset_child = manager.children(2)[0];
    REQUIRE(post_reset_child.generation != pre_reset_child.generation);
    REQUIRE(manager.release(2));
    REQUIRE_FALSE(manager.finish(pre_reset_child.voice_index, pre_reset_child.voice_id,
                                 pre_reset_child.generation));
    REQUIRE(manager.terminate_all(terms).terminated);
    REQUIRE_FALSE(manager.prepare(first));
}

TEST_CASE("Unison voice stack RT operations allocate no memory and reset modulation",
          "[audio][unison-stack][rt]") {
    InstrumentVoiceAllocator allocator;
    REQUIRE(allocator.prepare(4));
    UnisonVoiceStackManager<> manager;
    REQUIRE(manager.prepare(allocator));
    std::array<UnisonVoiceTermination, 4> terms{};
    std::array<VoiceModulationBuffer, 4> modulation{};
    for (auto& buffer : modulation)
        REQUIRE(buffer.prepare({.max_lanes = 2, .max_frames = 16}));
    pulp::test::RtAllocationProbe probe;
    const auto result = manager.trigger(
        {.source_id = 1, .voice_count = 2, .note = 60, .sample_id = 1}, terms, modulation);
    REQUIRE(result.allocated);
    REQUIRE(manager.release(1));
    const auto finishing = manager.children(1)[0];
    REQUIRE(manager.finish(finishing.voice_index, finishing.voice_id, finishing.generation,
                           modulation));
    REQUIRE(manager
                .trigger({.source_id = 2, .voice_count = 3, .note = 62, .sample_id = 2}, terms,
                         modulation)
                .allocated);
    REQUIRE(manager
                .trigger({.source_id = 3, .voice_count = 4, .note = 64, .sample_id = 3}, terms,
                         modulation)
                .allocated);
    REQUIRE(manager.terminate_all(terms, modulation).terminated);
    REQUIRE(manager.reset(modulation));
    REQUIRE_FALSE(probe.saw_allocation());
}
