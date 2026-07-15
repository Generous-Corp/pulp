#include <catch2/catch_test_macros.hpp>

#include <pulp/audio/rt_safety_contract.hpp>

#include <array>
#include <cstddef>
#include <string_view>

using pulp::audio::RtSafetyClass;
using pulp::audio::RtSafetyContract;
using pulp::audio::find_sampler_looper_rt_safety_contract;
using pulp::audio::rt_safety_class_is_audio_dsp_safe;
using pulp::audio::rt_safety_class_may_be_called_from_realtime_thread;
using pulp::audio::rt_safety_class_name;
using pulp::audio::sampler_looper_rt_safety_contracts;

namespace {

const RtSafetyContract& require_contract(std::string_view component,
                                         std::string_view operation) {
    const auto* contract = find_sampler_looper_rt_safety_contract(component, operation);
    REQUIRE(contract != nullptr);
    return *contract;
}

struct RequiredContract {
    std::string_view component;
    std::string_view operation;
    RtSafetyClass safety_class = RtSafetyClass::ControlThreadOnly;
    bool audio_callback_allowed = false;
};

template<std::size_t Count>
void check_required_contracts(
    const std::array<RequiredContract, Count>& expected_contracts) {
    for (const auto& expected : expected_contracts) {
        const auto& contract = require_contract(expected.component, expected.operation);
        CHECK(contract.safety_class == expected.safety_class);
        CHECK(contract.audio_callback_allowed == expected.audio_callback_allowed);
    }
}

}  // namespace

TEST_CASE("Sampler RT safety class labels are stable",
          "[audio][sampler][rt-safety]") {
    CHECK(rt_safety_class_name(RtSafetyClass::AudioCallbackSafe) ==
          "audio-callback-safe");
    CHECK(rt_safety_class_name(RtSafetyClass::AudioCallbackSafeAfterPrepare) ==
          "audio-callback-safe-after-prepare");
    CHECK(rt_safety_class_name(RtSafetyClass::AudioCallbackSafeWithImmutableInputs) ==
          "audio-callback-safe-with-immutable-inputs");
    CHECK(rt_safety_class_name(RtSafetyClass::RealtimeTelemetryOnly) ==
          "realtime-telemetry-only");
    CHECK(rt_safety_class_name(RtSafetyClass::ControlThreadOnly) ==
          "control-thread-only");
    CHECK(rt_safety_class_name(RtSafetyClass::BackgroundThreadOnly) ==
          "background-thread-only");
    CHECK(rt_safety_class_name(RtSafetyClass::OfflineOnly) == "offline-only");

    CHECK(rt_safety_class_may_be_called_from_realtime_thread(
        RtSafetyClass::AudioCallbackSafe));
    CHECK(rt_safety_class_may_be_called_from_realtime_thread(
        RtSafetyClass::AudioCallbackSafeAfterPrepare));
    CHECK(rt_safety_class_may_be_called_from_realtime_thread(
        RtSafetyClass::AudioCallbackSafeWithImmutableInputs));
    CHECK(rt_safety_class_may_be_called_from_realtime_thread(
        RtSafetyClass::RealtimeTelemetryOnly));
    CHECK_FALSE(rt_safety_class_may_be_called_from_realtime_thread(
        RtSafetyClass::ControlThreadOnly));
    CHECK_FALSE(rt_safety_class_may_be_called_from_realtime_thread(
        RtSafetyClass::BackgroundThreadOnly));
    CHECK_FALSE(rt_safety_class_may_be_called_from_realtime_thread(RtSafetyClass::OfflineOnly));

    CHECK(rt_safety_class_is_audio_dsp_safe(RtSafetyClass::AudioCallbackSafe));
    CHECK(rt_safety_class_is_audio_dsp_safe(
        RtSafetyClass::AudioCallbackSafeAfterPrepare));
    CHECK(rt_safety_class_is_audio_dsp_safe(
        RtSafetyClass::AudioCallbackSafeWithImmutableInputs));
    CHECK_FALSE(rt_safety_class_is_audio_dsp_safe(RtSafetyClass::RealtimeTelemetryOnly));
    CHECK_FALSE(rt_safety_class_is_audio_dsp_safe(RtSafetyClass::ControlThreadOnly));
    CHECK_FALSE(rt_safety_class_is_audio_dsp_safe(RtSafetyClass::BackgroundThreadOnly));
    CHECK_FALSE(rt_safety_class_is_audio_dsp_safe(RtSafetyClass::OfflineOnly));
}

TEST_CASE("Sampler RT safety contracts are well formed",
          "[audio][sampler][rt-safety]") {
    const auto contracts = sampler_looper_rt_safety_contracts();
    REQUIRE_FALSE(contracts.empty());

    for (const auto& contract : contracts) {
        CHECK_FALSE(contract.component.empty());
        CHECK_FALSE(contract.operation.empty());
        CHECK_FALSE(contract.owner_boundary.empty());
        CHECK_FALSE(contract.notes.empty());

        if (rt_safety_class_is_audio_dsp_safe(contract.safety_class)) {
            CHECK(contract.audio_callback_allowed);
        }

        if (contract.audio_callback_allowed) {
            CHECK(rt_safety_class_may_be_called_from_realtime_thread(
                contract.safety_class));
            CHECK_FALSE(contract.may_allocate);
            CHECK_FALSE(contract.may_lock);
            CHECK_FALSE(contract.may_block);
        }

        if (contract.may_lock) {
            CHECK(contract.may_block);
        }
    }

    for (std::size_t outer = 0; outer < contracts.size(); ++outer) {
        for (std::size_t inner = outer + 1; inner < contracts.size(); ++inner) {
            const bool same_key = contracts[outer].component == contracts[inner].component &&
                                  contracts[outer].operation == contracts[inner].operation;
            CHECK_FALSE(same_key);
        }
    }
}

TEST_CASE("Sampler RT safety contracts preserve required coverage",
          "[audio][sampler][rt-safety]") {
    constexpr std::array<RequiredContract, 15> hot_paths{{
        {"PlanarAudioRingBuffer", "read_write", RtSafetyClass::AudioCallbackSafeAfterPrepare, true},
        {"AudioStreamHandoff", "push_pull", RtSafetyClass::AudioCallbackSafeAfterPrepare, true},
        {"RollingAudioCaptureBuffer", "append_snapshot_hold", RtSafetyClass::AudioCallbackSafeAfterPrepare, true},
        {"RealtimeSampleRecorder", "enqueue_pop_process", RtSafetyClass::AudioCallbackSafeAfterPrepare, true},
        {"SampleSlotBank", "read_published_view", RtSafetyClass::AudioCallbackSafeWithImmutableInputs, true},
        {"SampleZoneMap", "select", RtSafetyClass::AudioCallbackSafeWithImmutableInputs, true},
        {"SamplePool", "resolve", RtSafetyClass::AudioCallbackSafeWithImmutableInputs, true},
        {"InstrumentRuntime", "trigger", RtSafetyClass::AudioCallbackSafeWithImmutableInputs, true},
        {"InstrumentVoiceAllocator", "trigger_release_finish", RtSafetyClass::AudioCallbackSafeAfterPrepare, true},
        {"AhdsrEnvelope", "note_render", RtSafetyClass::AudioCallbackSafeAfterPrepare, true},
        {"VoiceModulationBuffer", "begin_add_read", RtSafetyClass::AudioCallbackSafeAfterPrepare, true},
        {"SampleVoiceRenderer", "render", RtSafetyClass::AudioCallbackSafeWithImmutableInputs, true},
        {"SampleStreamWindow", "read_frames", RtSafetyClass::AudioCallbackSafeWithImmutableInputs, true},
        {"LoopReader", "read_validated", RtSafetyClass::AudioCallbackSafeWithImmutableInputs, true},
        {"VoiceSumMixer", "mix", RtSafetyClass::AudioCallbackSafe, true},
    }};

    constexpr std::array<RequiredContract, 12> off_thread_paths{{
        {"RealtimeSampleRecorder", "materialize_snapshot", RtSafetyClass::BackgroundThreadOnly, false},
        {"RealtimeSampleRecorder", "release_reset_consume", RtSafetyClass::OfflineOnly, false},
        {"SampleSlotBank", "publish_from_buffer", RtSafetyClass::ControlThreadOnly, false},
        {"PublishedSampleStore", "load_publish", RtSafetyClass::ControlThreadOnly, false},
        {"LoopPointAnalyzer", "analyze", RtSafetyClass::BackgroundThreadOnly, false},
        {"OnsetDetector", "detect", RtSafetyClass::BackgroundThreadOnly, false},
        {"SlicePointAnalyzer", "analyze", RtSafetyClass::BackgroundThreadOnly, false},
        {"TimePitchProcessor", "prepare_process_release", RtSafetyClass::BackgroundThreadOnly, false},
        {"SampleAssetImporter", "import", RtSafetyClass::BackgroundThreadOnly, false},
        {"SampleAssetExporter", "export", RtSafetyClass::BackgroundThreadOnly, false},
        {"SampleSlotMaterializer", "publish_completed_recording", RtSafetyClass::BackgroundThreadOnly, false},
        {"WaveformOverview", "build_and_cache", RtSafetyClass::BackgroundThreadOnly, false},
    }};

    check_required_contracts(hot_paths);
    check_required_contracts(off_thread_paths);
}

TEST_CASE("Sampler RT safety contracts pin expected hot paths",
          "[audio][sampler][rt-safety]") {
    const auto& ring = require_contract("PlanarAudioRingBuffer", "read_write");
    CHECK(ring.safety_class == RtSafetyClass::AudioCallbackSafeAfterPrepare);
    CHECK(ring.audio_callback_allowed);
    CHECK(ring.requires_prepare);

    const auto& recorder = require_contract("RealtimeSampleRecorder",
                                            "enqueue_pop_process");
    CHECK(recorder.safety_class == RtSafetyClass::AudioCallbackSafeAfterPrepare);
    CHECK(recorder.audio_callback_allowed);
    CHECK(recorder.requires_prepare);

    const auto& zone_select = require_contract("SampleZoneMap", "select");
    CHECK(zone_select.safety_class == RtSafetyClass::AudioCallbackSafeWithImmutableInputs);
    CHECK(zone_select.audio_callback_allowed);
    CHECK_FALSE(zone_select.may_allocate);

    const auto& sample_read = require_contract("PublishedSampleView", "read");
    CHECK(sample_read.safety_class == RtSafetyClass::AudioCallbackSafeWithImmutableInputs);
    CHECK(sample_read.audio_callback_allowed);
    CHECK_FALSE(sample_read.requires_prepare);

    const auto& loop_fast_path = require_contract("LoopReader", "read_validated");
    CHECK(loop_fast_path.audio_callback_allowed);
    CHECK(loop_fast_path.safety_class == RtSafetyClass::AudioCallbackSafeWithImmutableInputs);

    const auto& voice_mix = require_contract("VoiceSumMixer", "mix");
    CHECK(voice_mix.safety_class == RtSafetyClass::AudioCallbackSafe);
    CHECK(voice_mix.audio_callback_allowed);
}

TEST_CASE("Sampler RT safety contracts keep slow paths out of the callback",
          "[audio][sampler][rt-safety]") {
    const auto& import = require_contract("SampleAssetImporter", "import");
    CHECK(import.safety_class == RtSafetyClass::BackgroundThreadOnly);
    CHECK_FALSE(import.audio_callback_allowed);
    CHECK(import.may_allocate);
    CHECK(import.may_block);

    const auto& onset = require_contract("OnsetDetector", "detect");
    CHECK(onset.safety_class == RtSafetyClass::BackgroundThreadOnly);
    CHECK_FALSE(onset.audio_callback_allowed);
    CHECK(onset.may_allocate);

    const auto& publish = require_contract("PublishedSampleStore", "load_publish");
    CHECK(publish.safety_class == RtSafetyClass::ControlThreadOnly);
    CHECK_FALSE(publish.audio_callback_allowed);
    CHECK(publish.may_allocate);
    CHECK(publish.may_lock);

    const auto& recorder_release = require_contract("RealtimeSampleRecorder",
                                                    "release_reset_consume");
    CHECK(recorder_release.safety_class == RtSafetyClass::OfflineOnly);
    CHECK_FALSE(recorder_release.audio_callback_allowed);

    const auto& thumbnail = require_contract("WaveformOverview", "build_and_cache");
    CHECK(thumbnail.safety_class == RtSafetyClass::BackgroundThreadOnly);
    CHECK_FALSE(thumbnail.audio_callback_allowed);
    CHECK(thumbnail.may_allocate);
    CHECK(thumbnail.may_block);
}
