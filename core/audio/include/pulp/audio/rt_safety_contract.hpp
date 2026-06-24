#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>
#include <string_view>

namespace pulp::audio {

enum class RtSafetyClass : std::uint8_t {
    AudioCallbackSafe,
    AudioCallbackSafeAfterPrepare,
    AudioCallbackSafeWithImmutableInputs,
    // Bounded reporting/counter operations that may run on the realtime thread,
    // but are not signal-processing or mutable sampler-control operations.
    RealtimeTelemetryOnly,
    ControlThreadOnly,
    BackgroundThreadOnly,
    OfflineOnly,
};

struct RtSafetyContract {
    std::string_view component;
    std::string_view operation;
    RtSafetyClass safety_class = RtSafetyClass::ControlThreadOnly;
    bool audio_callback_allowed = false;
    bool may_allocate = false;
    bool may_lock = false;
    bool may_block = false;
    bool requires_prepare = false;
    std::string_view owner_boundary;
    std::string_view notes;
};

[[nodiscard]] constexpr std::string_view rt_safety_class_name(
    RtSafetyClass safety_class) noexcept {
    switch (safety_class) {
        case RtSafetyClass::AudioCallbackSafe:
            return "audio-callback-safe";
        case RtSafetyClass::AudioCallbackSafeAfterPrepare:
            return "audio-callback-safe-after-prepare";
        case RtSafetyClass::AudioCallbackSafeWithImmutableInputs:
            return "audio-callback-safe-with-immutable-inputs";
        case RtSafetyClass::RealtimeTelemetryOnly:
            return "realtime-telemetry-only";
        case RtSafetyClass::ControlThreadOnly:
            return "control-thread-only";
        case RtSafetyClass::BackgroundThreadOnly:
            return "background-thread-only";
        case RtSafetyClass::OfflineOnly:
            return "offline-only";
    }
    return "unknown";
}

[[nodiscard]] constexpr bool rt_safety_class_may_be_called_from_realtime_thread(
    RtSafetyClass safety_class) noexcept {
    switch (safety_class) {
        case RtSafetyClass::AudioCallbackSafe:
        case RtSafetyClass::AudioCallbackSafeAfterPrepare:
        case RtSafetyClass::AudioCallbackSafeWithImmutableInputs:
        case RtSafetyClass::RealtimeTelemetryOnly:
            return true;
        case RtSafetyClass::ControlThreadOnly:
        case RtSafetyClass::BackgroundThreadOnly:
        case RtSafetyClass::OfflineOnly:
            return false;
    }
    return false;
}

[[nodiscard]] constexpr bool rt_safety_class_is_audio_dsp_safe(
    RtSafetyClass safety_class) noexcept {
    switch (safety_class) {
        case RtSafetyClass::AudioCallbackSafe:
        case RtSafetyClass::AudioCallbackSafeAfterPrepare:
        case RtSafetyClass::AudioCallbackSafeWithImmutableInputs:
            return true;
        case RtSafetyClass::RealtimeTelemetryOnly:
        case RtSafetyClass::ControlThreadOnly:
        case RtSafetyClass::BackgroundThreadOnly:
        case RtSafetyClass::OfflineOnly:
            return false;
    }
    return false;
}

namespace detail {

[[nodiscard]] constexpr RtSafetyContract make_rt_safety_contract(
    std::string_view component,
    std::string_view operation,
    RtSafetyClass safety_class,
    bool audio_callback_allowed,
    bool may_allocate,
    bool may_lock,
    bool may_block,
    bool requires_prepare,
    std::string_view owner_boundary,
    std::string_view notes) noexcept {
    return RtSafetyContract{component,
                            operation,
                            safety_class,
                            audio_callback_allowed,
                            may_allocate,
                            may_lock,
                            may_block,
                            requires_prepare,
                            owner_boundary,
                            notes};
}

}  // namespace detail

inline constexpr std::array kSamplerLooperRtSafetyContracts{
    detail::make_rt_safety_contract(
        "PlanarAudioRingBuffer",
        "read_write",
        RtSafetyClass::AudioCallbackSafeAfterPrepare,
        true,
        false,
        false,
        false,
        true,
        "one source producer and one host consumer",
        "Prepared fixed storage; short reads zero-fill and short writes drop excess frames."),
    detail::make_rt_safety_contract(
        "AudioStreamHandoff",
        "push_pull",
        RtSafetyClass::AudioCallbackSafeAfterPrepare,
        true,
        false,
        false,
        false,
        true,
        "one generated-audio producer and one host consumer",
        "Prepared resampler handoff; underruns are zero-filled."),
    detail::make_rt_safety_contract(
        "RollingAudioCaptureBuffer",
        "append_snapshot_hold",
        RtSafetyClass::AudioCallbackSafeAfterPrepare,
        true,
        false,
        false,
        false,
        true,
        "single capture owner",
        "Append/snapshot/hold are owner-thread operations; held appends are discarded and counted."),
    detail::make_rt_safety_contract(
        "RollingAudioCaptureBuffer",
        "materialize",
        RtSafetyClass::BackgroundThreadOnly,
        false,
        false,
        false,
        false,
        false,
        "capture owner or paused external materializer",
        "Materialization copies held or quiescent storage and must not race live append."),
    detail::make_rt_safety_contract(
        "RealtimeSampleRecorder",
        "enqueue_pop_process",
        RtSafetyClass::AudioCallbackSafeAfterPrepare,
        true,
        false,
        false,
        false,
        true,
        "one command producer, one event consumer, one audio processor",
        "Prepared queues/storage; commands are block-offset sorted inside process()."),
    detail::make_rt_safety_contract(
        "RealtimeSampleRecorder",
        "materialize_snapshot",
        RtSafetyClass::BackgroundThreadOnly,
        false,
        false,
        false,
        false,
        false,
        "paused capture owner or active hold owner",
        "Copies rolling-capture data into recorder storage outside the callback."),
    detail::make_rt_safety_contract(
        "RealtimeSampleRecorder",
        "release_reset_consume",
        RtSafetyClass::OfflineOnly,
        false,
        false,
        false,
        false,
        false,
        "offline owner",
        "Do not call concurrently with process(); use queued reset for live control."),
    detail::make_rt_safety_contract(
        "SampleSlotBank",
        "publish_from_buffer",
        RtSafetyClass::ControlThreadOnly,
        false,
        false,
        false,
        false,
        true,
        "single serialized publication owner",
        "Prepared fixed slots are mutated off callback and exposed through generation-safe views."),
    detail::make_rt_safety_contract(
        "SampleSlotBank",
        "read_published_view",
        RtSafetyClass::AudioCallbackSafeWithImmutableInputs,
        true,
        false,
        false,
        false,
        true,
        "audio reader with externally retained slot generations",
        "Audio code reads PublishedSampleView snapshots and generation-aware channel pointers only."),
    detail::make_rt_safety_contract(
        "PublishedSampleStore",
        "load_publish",
        RtSafetyClass::ControlThreadOnly,
        false,
        true,
        true,
        true,
        true,
        "controller, importer, or background publication owner",
        "Load/publish may copy, allocate, block, and take the store mutex."),
    detail::make_rt_safety_contract(
        "PublishedSampleView",
        "read",
        RtSafetyClass::AudioCallbackSafeWithImmutableInputs,
        true,
        false,
        false,
        false,
        false,
        "generation-pinned audio reader",
        "Borrowed channel pointers remain valid only while the published generation is retained."),
    detail::make_rt_safety_contract(
        "SampleZoneMap",
        "configure",
        RtSafetyClass::ControlThreadOnly,
        false,
        true,
        false,
        false,
        false,
        "mapping owner",
        "Copies zone metadata and publishes whole-map snapshots for realtime readers."),
    detail::make_rt_safety_contract(
        "SampleZoneMap",
        "select",
        RtSafetyClass::AudioCallbackSafeWithImmutableInputs,
        true,
        false,
        false,
        false,
        false,
        "audio reader over immutable map snapshot",
        "Selection owns no sample storage and performs bounded metadata lookup."),
    detail::make_rt_safety_contract(
        "SamplePool",
        "resolve",
        RtSafetyClass::AudioCallbackSafeWithImmutableInputs,
        true,
        false,
        false,
        false,
        false,
        "audio reader over immutable pool snapshot",
        "Borrowed PublishedSampleStore instances must outlive realtime readers."),
    detail::make_rt_safety_contract(
        "InstrumentRuntime",
        "trigger",
        RtSafetyClass::AudioCallbackSafeWithImmutableInputs,
        true,
        false,
        false,
        false,
        false,
        "audio trigger path with immutable zone map and sample pool",
        "Resolves pool-backed zones and playback rate without allocating voices."),
    detail::make_rt_safety_contract(
        "InstrumentVoiceAllocator",
        "trigger_release_finish",
        RtSafetyClass::AudioCallbackSafeAfterPrepare,
        true,
        false,
        false,
        false,
        true,
        "audio-owned prepared voice slots",
        "Mutates only the prepared voice array; publish telemetry through separate queues."),
    detail::make_rt_safety_contract(
        "AhdsrEnvelope",
        "note_render",
        RtSafetyClass::AudioCallbackSafeAfterPrepare,
        true,
        false,
        false,
        false,
        true,
        "per-voice envelope owner",
        "Prepared envelope state renders gains into caller-owned buffers."),
    detail::make_rt_safety_contract(
        "VoiceModulationBuffer",
        "begin_add_read",
        RtSafetyClass::AudioCallbackSafeAfterPrepare,
        true,
        false,
        false,
        false,
        true,
        "per-voice or per-block modulation owner",
        "Prepared lane/value storage is reused for each block."),
    detail::make_rt_safety_contract(
        "SampleVoiceRenderer",
        "render",
        RtSafetyClass::AudioCallbackSafeWithImmutableInputs,
        true,
        false,
        false,
        false,
        false,
        "voice render owner with borrowed sample view and caller-owned scratch",
        "Renders scalar one-shot or region playback into caller-owned output buffers."),
    detail::make_rt_safety_contract(
        "VoiceSumMixer",
        "mix",
        RtSafetyClass::AudioCallbackSafe,
        true,
        false,
        false,
        false,
        false,
        "caller-owned voice scratch and output buffers",
        "Static mixer over non-overlapping source and destination buffers."),
    detail::make_rt_safety_contract(
        "SampleStreamWindow",
        "read_frames",
        RtSafetyClass::AudioCallbackSafeWithImmutableInputs,
        true,
        false,
        false,
        false,
        true,
        "audio reader with generation-gated page lifetimes",
        "Reads resident prepared pages only; never loads, fills, evicts, or performs file I/O."),
    detail::make_rt_safety_contract(
        "LoopReader",
        "read_validated",
        RtSafetyClass::AudioCallbackSafeWithImmutableInputs,
        true,
        false,
        false,
        false,
        false,
        "renderer that already validated source region for the block",
        "Unchecked fast path for hot loops; public read() remains the guard rail."),
    detail::make_rt_safety_contract(
        "LoopRenderer",
        "render",
        RtSafetyClass::AudioCallbackSafeWithImmutableInputs,
        true,
        false,
        false,
        false,
        false,
        "voice or loop render owner with immutable source buffer",
        "Stateful overwrite renderer for caller-owned scratch/output buffers."),
    detail::make_rt_safety_contract(
        "LoopPointAnalyzer",
        "analyze",
        RtSafetyClass::BackgroundThreadOnly,
        false,
        true,
        false,
        false,
        false,
        "editor, importer, or analysis job",
        "May inspect long windows and run candidate search outside Processor::process()."),
    detail::make_rt_safety_contract(
        "OnsetDetector",
        "detect",
        RtSafetyClass::BackgroundThreadOnly,
        false,
        true,
        false,
        false,
        false,
        "editor, importer, or analysis job",
        "Produces onset markers for slice analysis outside package-specific APIs."),
    detail::make_rt_safety_contract(
        "SlicePointAnalyzer",
        "analyze",
        RtSafetyClass::BackgroundThreadOnly,
        false,
        true,
        false,
        false,
        false,
        "editor, importer, or analysis job",
        "Combines transient/beat/manual/imported markers into slice maps off callback."),
    detail::make_rt_safety_contract(
        "TimePitchProcessor",
        "prepare_process_release",
        RtSafetyClass::BackgroundThreadOnly,
        false,
        true,
        true,
        true,
        true,
        "offline render, bounce, or analysis owner",
        "Package-backed processors are optional and may initialize package/platform internals."),
    detail::make_rt_safety_contract(
        "SampleAssetImporter",
        "import",
        RtSafetyClass::BackgroundThreadOnly,
        false,
        true,
        true,
        true,
        false,
        "importer or platform adapter",
        "May do file I/O, codec work, locks, and decoded-audio allocation."),
    detail::make_rt_safety_contract(
        "SampleAssetExporter",
        "export",
        RtSafetyClass::BackgroundThreadOnly,
        false,
        true,
        true,
        true,
        false,
        "exporter or platform adapter",
        "May do file I/O, codec work, locks, and encoded-audio allocation."),
    detail::make_rt_safety_contract(
        "SampleSlotMaterializer",
        "publish_completed_recording",
        RtSafetyClass::BackgroundThreadOnly,
        false,
        false,
        false,
        false,
        false,
        "publication owner with recorder and slot bank quiesced as required",
        "Moves completed recorder data into prepared slots outside the callback."),
    detail::make_rt_safety_contract(
        "AudioThumbnail",
        "build_and_cache",
        RtSafetyClass::BackgroundThreadOnly,
        false,
        true,
        true,
        true,
        false,
        "waveform analysis/cache owner",
        "Builds peaks, serializes cache payloads, and may read files, lock, or allocate."),
};

[[nodiscard]] constexpr std::span<const RtSafetyContract>
sampler_looper_rt_safety_contracts() noexcept {
    return kSamplerLooperRtSafetyContracts;
}

[[nodiscard]] constexpr const RtSafetyContract* find_sampler_looper_rt_safety_contract(
    std::string_view component,
    std::string_view operation) noexcept {
    for (const auto& contract : kSamplerLooperRtSafetyContracts) {
        if (contract.component == component && contract.operation == operation) {
            return &contract;
        }
    }
    return nullptr;
}

// Core realtime-runtime callback-boundary contracts. Companion to the
// sampler/looper table above, extending the codified RT-safety registry beyond
// the sampler to the lock-free primitives, the per-block automation queue, the
// host graph walk, the load-telemetry counter, the denormal-mode guard, and
// the Processor DSP entry point. These labels are DESCRIPTIVE — the actual
// enforcement is the no-alloc/no-lock abort-trap in
// test/native_components/rt_intercept_test_support.cpp plus the per-primitive
// no-alloc tests; the same well-formedness invariants the sampler table is
// drift-checked against (see test_sampler_rt_safety_contract.cpp) apply here
// and are asserted in test_core_runtime_rt_safety_contract.cpp.
inline constexpr std::array kCoreRuntimeRtSafetyContracts{
    detail::make_rt_safety_contract(
        "SeqLock",
        "read",
        RtSafetyClass::AudioCallbackSafe,
        true,
        false,
        false,
        false,
        false,
        "one writer thread; any number of reader threads",
        "Lock-free coherent snapshot via acquire/release seq counter; readers "
        "bounded-spin-retry on a concurrent write, never block on a lock."),
    detail::make_rt_safety_contract(
        "SeqLock",
        "write",
        RtSafetyClass::AudioCallbackSafe,
        true,
        false,
        false,
        false,
        false,
        "single writer thread",
        "Byte-wise volatile copy bracketed by an odd/even seq increment; cost "
        "scales with sizeof(T), so keep T small for audio-thread writers."),
    detail::make_rt_safety_contract(
        "TripleBuffer",
        "read_write",
        RtSafetyClass::AudioCallbackSafe,
        true,
        false,
        false,
        false,
        false,
        "one writer thread and one reader thread",
        "Lock-free latest-value publication via a dirty-bit CAS; the writer "
        "copies whole T into the back buffer, so bound sizeof(T)."),
    detail::make_rt_safety_contract(
        "SpscQueue",
        "try_push_pop",
        RtSafetyClass::AudioCallbackSafe,
        true,
        false,
        false,
        false,
        false,
        "one producer thread and one consumer thread",
        "Wraps choc SingleReaderSingleWriterFIFO; fixed capacity, overflow is "
        "counted and the push fails rather than allocating."),
    detail::make_rt_safety_contract(
        "ParameterEventQueue",
        "push_sort_clear",
        RtSafetyClass::AudioCallbackSafe,
        true,
        false,
        false,
        false,
        false,
        "per-block automation owner on the audio thread",
        "Fixed-capacity (kCapacity) std::array storage; push/sort/clear never "
        "allocate, overflow is dropped and counted. sort() is a stable "
        "insertion sort tuned for near-sorted host automation."),
    detail::make_rt_safety_contract(
        "AudioProcessLoadMeasurer",
        "begin_end",
        RtSafetyClass::RealtimeTelemetryOnly,
        true,
        false,
        false,
        false,
        false,
        "one measuring thread; any number of polling threads",
        "steady_clock delta plus relaxed-atomic latest-value stores; emits "
        "telemetry only, performs no signal processing or control mutation."),
    detail::make_rt_safety_contract(
        "ScopedFlushDenormals",
        "scope",
        RtSafetyClass::AudioCallbackSafe,
        true,
        false,
        false,
        false,
        false,
        "audio callback boundary owner",
        "Sets the CPU flush-to-zero FP mode (MXCSR FTZ / FPCR.FZ) on entry and "
        "restores the caller's mode on exit; no allocation, lock, or syscall."),
    detail::make_rt_safety_contract(
        "SignalGraph",
        "process",
        RtSafetyClass::AudioCallbackSafeAfterPrepare,
        true,
        false,
        false,
        false,
        true,
        "audio thread over a prepared, published CompiledGraph snapshot",
        "Loads the live snapshot via an atomic raw pointer under a reader-count "
        "guard (deliberately not atomic<shared_ptr>), then walks ordered_runtime "
        "single-threaded; topology mutation happens off the callback."),
    detail::make_rt_safety_contract(
        "Processor",
        "process",
        RtSafetyClass::AudioCallbackSafeAfterPrepare,
        true,
        false,
        false,
        false,
        true,
        "audio thread; format adapter sets RT sidecars before the call",
        "The plugin DSP entry point: no allocation, lock, I/O, or exceptions. "
        "Adapters wrap it in ScopedNoAlloc; prepare() must run first."),
    detail::make_rt_safety_contract(
        "GraphRuntimeExecutor",
        "process_routed",
        RtSafetyClass::AudioCallbackSafeAfterPrepare,
        true,
        false,
        false,
        false,
        true,
        "audio thread over a prepared snapshot + pre-sized GraphRuntimeBufferPool",
        "Routes inter-node audio through caller-allocated scratch slots: "
        "gather/scatter and the one-block feedback capture are bounded fills and "
        "copies, no allocation or lock. The snapshot's buffer assignment and the "
        "pool are built off the callback; pool.fits() must hold for the block."),
};

[[nodiscard]] constexpr std::span<const RtSafetyContract>
core_runtime_rt_safety_contracts() noexcept {
    return kCoreRuntimeRtSafetyContracts;
}

[[nodiscard]] constexpr const RtSafetyContract* find_core_runtime_rt_safety_contract(
    std::string_view component,
    std::string_view operation) noexcept {
    for (const auto& contract : kCoreRuntimeRtSafetyContracts) {
        if (contract.component == component && contract.operation == operation) {
            return &contract;
        }
    }
    return nullptr;
}

}  // namespace pulp::audio
