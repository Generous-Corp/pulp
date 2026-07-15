// Drift check for the core-runtime RT-safety contract registry
// (kCoreRuntimeRtSafetyContracts in rt_safety_contract.hpp). Mirrors the
// well-formedness invariants the sampler/looper table is checked against in
// test_sampler_rt_safety_contract.cpp, so a future edit that contradicts the
// classification (e.g. marking something AudioCallbackSafe while flagging
// may_lock) fails CI instead of silently drifting.

#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/rt_safety_contract.hpp>

#include <string_view>

using pulp::audio::RtSafetyClass;
using pulp::audio::core_runtime_rt_safety_contracts;
using pulp::audio::find_core_runtime_rt_safety_contract;
using pulp::audio::rt_safety_class_is_audio_dsp_safe;
using pulp::audio::rt_safety_class_may_be_called_from_realtime_thread;

TEST_CASE("Core runtime RT safety contracts are well formed",
          "[audio][rt-safety][contract][drift]") {
    const auto contracts = core_runtime_rt_safety_contracts();
    REQUIRE_FALSE(contracts.empty());

    for (const auto& contract : contracts) {
        CAPTURE(contract.component);
        CAPTURE(contract.operation);

        // Every entry must be fully labeled.
        CHECK_FALSE(contract.component.empty());
        CHECK_FALSE(contract.operation.empty());
        CHECK_FALSE(contract.owner_boundary.empty());
        CHECK_FALSE(contract.notes.empty());

        // A DSP-safe class must be callable from the audio callback.
        if (rt_safety_class_is_audio_dsp_safe(contract.safety_class)) {
            CHECK(contract.audio_callback_allowed);
        }

        // Anything callable from the audio callback must not allocate, lock,
        // or block — the core no-alloc/no-lock contract.
        if (contract.audio_callback_allowed) {
            CHECK_FALSE(contract.may_allocate);
            CHECK_FALSE(contract.may_lock);
            CHECK_FALSE(contract.may_block);
            // The class itself must agree that it can run on the RT thread.
            CHECK(rt_safety_class_may_be_called_from_realtime_thread(
                contract.safety_class));
        }

        // A lock implies a potential block.
        if (contract.may_lock) {
            CHECK(contract.may_block);
        }

        // requires_prepare is the only flag with no other cross-field
        // invariant, so tie it bidirectionally to the AfterPrepare class: a
        // transposed bool would otherwise be a silent mislabel CI can't catch.
        if (contract.requires_prepare) {
            CHECK(contract.safety_class == RtSafetyClass::AudioCallbackSafeAfterPrepare);
        }
        if (contract.safety_class == RtSafetyClass::AudioCallbackSafeAfterPrepare) {
            CHECK(contract.requires_prepare);
        }
    }

    // Component+operation pairs must be unique (no shadowed registry rows).
    for (std::size_t outer = 0; outer < contracts.size(); ++outer) {
        for (std::size_t inner = outer + 1; inner < contracts.size(); ++inner) {
            const bool same = contracts[outer].component == contracts[inner].component
                && contracts[outer].operation == contracts[inner].operation;
            CHECK_FALSE(same);
        }
    }
}

TEST_CASE("Core runtime RT safety contracts pin expected classifications",
          "[audio][rt-safety][contract]") {
    // Lock-free primitives are callback-safe with no prepare requirement.
    const auto* seqlock = find_core_runtime_rt_safety_contract("SeqLock", "read");
    REQUIRE(seqlock != nullptr);
    CHECK(seqlock->safety_class == RtSafetyClass::AudioCallbackSafe);
    CHECK(seqlock->audio_callback_allowed);
    CHECK_FALSE(seqlock->requires_prepare);

    const auto* spsc =
        find_core_runtime_rt_safety_contract("SpscQueue", "try_push_pop");
    REQUIRE(spsc != nullptr);
    CHECK(spsc->audio_callback_allowed);
    CHECK_FALSE(spsc->may_allocate);

    const auto* slot = find_core_runtime_rt_safety_contract("RealtimeResourceSlot", "get");
    REQUIRE(slot != nullptr);
    CHECK(slot->audio_callback_allowed);
    CHECK_FALSE(slot->requires_prepare);
    CHECK_FALSE(slot->may_allocate);
    CHECK_FALSE(slot->may_lock);

    // The load measurer is telemetry-only: callback-allowed but NOT dsp-safe.
    const auto* load =
        find_core_runtime_rt_safety_contract("AudioProcessLoadMeasurer", "begin_end");
    REQUIRE(load != nullptr);
    CHECK(load->safety_class == RtSafetyClass::RealtimeTelemetryOnly);
    CHECK(load->audio_callback_allowed);
    CHECK_FALSE(rt_safety_class_is_audio_dsp_safe(load->safety_class));

    // The host graph walk and the Processor DSP entry require prepare() first.
    const auto* graph =
        find_core_runtime_rt_safety_contract("SignalGraph", "process");
    REQUIRE(graph != nullptr);
    CHECK(graph->safety_class == RtSafetyClass::AudioCallbackSafeAfterPrepare);
    CHECK(graph->requires_prepare);

    const auto* proc = find_core_runtime_rt_safety_contract("Processor", "process");
    REQUIRE(proc != nullptr);
    CHECK(proc->audio_callback_allowed);
    CHECK(proc->requires_prepare);

    // Unknown lookups return null rather than a stale row.
    CHECK(find_core_runtime_rt_safety_contract("SeqLock", "nope") == nullptr);
    CHECK(find_core_runtime_rt_safety_contract("Nope", "read") == nullptr);
}
