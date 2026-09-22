#pragma once

#include "gpu_convolver_trial_config.hpp"

#include <algorithm>
#include <cstdint>
#include <iomanip>
#include <map>
#include <ostream>
#include <pulp/runtime/crypto.hpp>
#include <span>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_set>
#include <utility>
#include <vector>

namespace pulp::gpu_audio::detail {

// This is the host-only manifest surface for the strict campaign writer. The
// benchmark owns these values; the writer never guesses machine, provider, or
// build identity from a trace record.
struct GpuConvolverRawManifest {
    std::string campaign = "screening";
    std::string campaign_id;
    std::string source_revision;
    std::string binary_sha256;
    std::string machine_id;
    std::string machine_model;
    std::string os_version;
    std::string adapter_name;
    std::string adapter_backend;
    std::uint64_t adapter_registry_id = 0;
    std::uint64_t adapter_vendor_id = 0;
    std::uint64_t adapter_device_id = 0;
    std::string provider;
    std::string provider_revision;
    std::string provider_asset_sha256;
    std::string generated_utc;
    std::vector<std::string> build_flags;
    std::uint32_t warmup_blocks = 0;
    std::uint32_t expected_trials = 0;
    std::uint32_t expected_matched_pairs = 0;
    std::uint32_t expected_staged_sync_trials = 0;
    std::uint64_t bootstrap_seed = 0;
    std::uint32_t bootstrap_resamples = 0;
    bool paced = false;

    std::uint32_t block_frames = 0;
    std::uint32_t sample_rate_hz = 0;
    std::uint32_t channels = 0;
    std::uint32_t ir_frames = 0;
    std::uint32_t inflight_depth = 0;
    std::uint32_t lead_blocks = 0;
    std::uint64_t deadline_ns = 0;
    std::uint64_t watchdog_ns = 0;
    GpuConvolverTrialLoad load = GpuConvolverTrialLoad::Quiet;

    std::string confirmation_campaign_id;
    std::string confirmation_summary_sha256;
};

enum class GpuConvolverRawTrialPath : std::uint8_t {
    StagedSync,
    StagedAsync,
    SharedAsync,
};

struct GpuConvolverRawTrial {
    std::uint64_t trial_id = 0;
    std::uint64_t pair_id = 0;
    GpuConvolverRawTrialPath path = GpuConvolverRawTrialPath::SharedAsync;
    std::uint64_t engine_id = 0;
    std::uint64_t generation = 0;
    std::uint64_t ui_frame_p99_ns = 0;
    std::uint64_t duration_ns = 0;
    std::span<const SharedIoTraceRecord> records;
};

namespace raw_writer_detail {

struct Identity {
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    friend bool operator<(const Identity& left, const Identity& right) noexcept {
        return left.generation < right.generation ||
               (left.generation == right.generation && left.sequence < right.sequence);
    }
};

struct Pair {
    const SharedIoTraceRecord* terminal = nullptr;
    const SharedIoTraceRecord* delivery = nullptr;
};

inline bool nonempty(std::string_view value) noexcept {
    return !value.empty();
}

inline bool hex_string(std::string_view value, std::size_t length) noexcept {
    if (value.size() != length)
        return false;
    return std::all_of(value.begin(), value.end(), [](const char character) {
        return (character >= '0' && character <= '9') ||
               (character >= 'a' && character <= 'f') ||
               (character >= 'A' && character <= 'F');
    });
}

inline bool valid_load(GpuConvolverTrialLoad load) noexcept {
    return load == GpuConvolverTrialLoad::Quiet || load == GpuConvolverTrialLoad::GraphiteUi ||
           load == GpuConvolverTrialLoad::GpuContention || load == GpuConvolverTrialLoad::Overload;
}

inline const char* load_name(GpuConvolverTrialLoad load) noexcept {
    switch (load) {
    case GpuConvolverTrialLoad::Quiet:
        return "quiet";
    case GpuConvolverTrialLoad::GraphiteUi:
        return "graphite_ui";
    case GpuConvolverTrialLoad::GpuContention:
        return "gpu_contention";
    case GpuConvolverTrialLoad::Overload:
        return "overload";
    }
    return "unknown";
}

inline const char* path_name(GpuConvolverRawTrialPath path) noexcept {
    switch (path) {
    case GpuConvolverRawTrialPath::StagedSync:
        return "staged_sync";
    case GpuConvolverRawTrialPath::StagedAsync:
        return "staged_async";
    case GpuConvolverRawTrialPath::SharedAsync:
        return "shared_async";
    }
    return "unknown";
}

inline bool valid_path(GpuConvolverRawTrialPath path) noexcept {
    return path == GpuConvolverRawTrialPath::StagedSync ||
           path == GpuConvolverRawTrialPath::StagedAsync ||
           path == GpuConvolverRawTrialPath::SharedAsync;
}

inline void json_string(std::ostream& output, std::string_view value) {
    output << '"';
    for (const auto character : value) {
        switch (character) {
        case '"':
            output << "\\\"";
            break;
        case '\\':
            output << "\\\\";
            break;
        case '\b':
            output << "\\b";
            break;
        case '\f':
            output << "\\f";
            break;
        case '\n':
            output << "\\n";
            break;
        case '\r':
            output << "\\r";
            break;
        case '\t':
            output << "\\t";
            break;
        default:
            if (static_cast<unsigned char>(character) < 0x20) {
                output << "\\u" << std::hex << std::setw(4) << std::setfill('0')
                       << static_cast<unsigned>(static_cast<unsigned char>(character)) << std::dec
                       << std::setfill(' ');
            } else {
                output << character;
            }
            break;
        }
    }
    output << '"';
}

inline void observation(std::ostream& output, std::uint64_t value, std::string_view observer,
                        std::string_view api_source) {
    output << R"({"api_source":)";
    json_string(output, api_source);
    output << R"(,"availability":"available","callback_mode":")";
    output << (observer == "audio_callback" ? "realtime" : "non_realtime");
    output << R"(","clock_domain":"monotonic","correlation_method":"not_required",)"
              R"("end_clock_domain":"monotonic","end_observer":)";
    json_string(output, observer);
    output << R"(,"event_pump_strategy":")";
    output << (observer == "audio_callback" ? "none" : "worker_serialized");
    output << R"(","instrumentation_control":"opt_in_monotonic_clock",)"
              R"("instrumentation_overhead_ns":0,"observer":)";
    json_string(output, observer);
    output << R"(,"relation":"direct","start_clock_domain":"monotonic",)"
              R"("start_observer":)";
    json_string(output, observer);
    output << R"(,"timestamp_scope":"single_block","uncertainty_ns":0,"value_ns":)" << value
           << '}';
}

inline void unavailable_observation(std::ostream& output, std::string_view observer,
                                   std::string_view api_source) {
    output << R"({"api_source":)";
    json_string(output, api_source);
    output << R"(,"availability":"unavailable","callback_mode":"non_realtime",)"
              R"("clock_domain":"monotonic","correlation_method":"unavailable",)"
              R"("end_clock_domain":"monotonic","end_observer":)";
    json_string(output, observer);
    output << R"(,"event_pump_strategy":"not_observed",)"
              R"("instrumentation_control":"opt_in_monotonic_clock",)"
              R"("instrumentation_overhead_ns":null,"observer":)";
    json_string(output, observer);
    output << R"(,"relation":"unavailable","start_clock_domain":"monotonic",)"
              R"("start_observer":)";
    json_string(output, observer);
    output << R"(,"timestamp_scope":"single_block","uncertainty_ns":null,"value_ns":null})";
}

inline bool duration(const SharedIoTraceRecord& record, SharedIoTraceStage begin,
                    SharedIoTraceStage end, std::uint64_t& result) noexcept {
    const auto value = shared_io_trace_duration(record, begin, end);
    if (!value.available)
        return false;
    result = value.ns;
    return true;
}

inline const char* terminal_name(SharedIoGpuTerminalDisposition value) noexcept {
    switch (value) {
    case SharedIoGpuTerminalDisposition::CompletedAccepted:
        return "completed";
    case SharedIoGpuTerminalDisposition::StaleRejected:
        return "stale_rejected";
    case SharedIoGpuTerminalDisposition::LateRejected:
        return "late_rejected";
    case SharedIoGpuTerminalDisposition::ProviderFailed:
        return "provider_failure";
    case SharedIoGpuTerminalDisposition::DeviceLost:
        return "device_lost";
    case SharedIoGpuTerminalDisposition::CancelledTeardown:
        return "cancelled_teardown";
    case SharedIoGpuTerminalDisposition::None:
        break;
    }
    return nullptr;
}

inline const char* delivery_name(SharedIoDeliveryDisposition value) noexcept {
    switch (value) {
    case SharedIoDeliveryDisposition::GpuDelivered:
        return "gpu";
    case SharedIoDeliveryDisposition::CpuFallbackDelivered:
        return "cpu_fallback";
    case SharedIoDeliveryDisposition::SilenceDelivered:
        return "silence";
    case SharedIoDeliveryDisposition::PassthroughDelivered:
        return "passthrough";
    case SharedIoDeliveryDisposition::Priming:
        return "priming";
    case SharedIoDeliveryDisposition::None:
    case SharedIoDeliveryDisposition::InvalidRejected:
        break;
    }
    return nullptr;
}

inline bool valid_manifest(const GpuConvolverRawManifest& manifest) noexcept {
    if (manifest.campaign != "screening" && manifest.campaign != "confirmation" &&
        manifest.campaign != "default" && manifest.campaign != "overload")
        return false;
    for (const auto value : {std::string_view(manifest.campaign_id),
                             std::string_view(manifest.machine_id),
                             std::string_view(manifest.machine_model),
                             std::string_view(manifest.os_version),
                             std::string_view(manifest.adapter_name),
                             std::string_view(manifest.adapter_backend),
                             std::string_view(manifest.provider),
                             std::string_view(manifest.generated_utc)}) {
        if (!nonempty(value))
            return false;
    }
    if (!hex_string(manifest.source_revision, 40) ||
        !hex_string(manifest.provider_revision, 40) ||
        !hex_string(manifest.binary_sha256, 64) ||
        !hex_string(manifest.provider_asset_sha256, 64) ||
        manifest.build_flags.empty() || manifest.warmup_blocks == 0 ||
        manifest.expected_trials == 0 || manifest.expected_matched_pairs == 0 ||
        manifest.bootstrap_resamples < 100 || !manifest.paced || manifest.block_frames == 0 ||
        manifest.sample_rate_hz == 0 || manifest.channels == 0 || manifest.ir_frames == 0 ||
        manifest.inflight_depth == 0 || manifest.lead_blocks == 0 || manifest.deadline_ns == 0 ||
        manifest.watchdog_ns <= manifest.deadline_ns || !valid_load(manifest.load))
        return false;
    bool has_o3 = false;
    bool has_ndebug = false;
    for (const auto& flag : manifest.build_flags) {
        if (flag.empty())
            return false;
        has_o3 |= flag == "-O3";
        has_ndebug |= flag == "-DNDEBUG";
    }
    if (!has_o3 || !has_ndebug)
        return false;
    if (manifest.expected_trials !=
        manifest.expected_matched_pairs * 2u + manifest.expected_staged_sync_trials)
        return false;
    if ((manifest.campaign == "screening" || manifest.campaign == "confirmation") &&
        manifest.expected_staged_sync_trials != 1)
        return false;
    if (manifest.campaign == "confirmation" &&
        (manifest.expected_matched_pairs < 30 || manifest.bootstrap_resamples < 10'000))
        return false;
    if (manifest.campaign == "default" &&
        (manifest.block_frames < 1 || manifest.expected_trials < 2 ||
         manifest.confirmation_campaign_id.empty() ||
         !hex_string(manifest.confirmation_summary_sha256, 64)))
        return false;
    if (manifest.campaign == "overload" && manifest.load != GpuConvolverTrialLoad::Overload)
        return false;
    return true;
}

inline bool valid_trial(const GpuConvolverRawManifest& manifest,
                        const GpuConvolverRawTrial& trial) noexcept {
    if (trial.trial_id == 0 || !valid_path(trial.path) || trial.engine_id == 0 ||
        trial.generation == 0 || trial.ui_frame_p99_ns == 0 || trial.duration_ns == 0 ||
        trial.records.empty())
        return false;
    if (trial.path == GpuConvolverRawTrialPath::StagedSync && trial.pair_id != 0)
        return false;
    if (trial.path != GpuConvolverRawTrialPath::StagedSync && trial.pair_id == 0)
        return false;
    if (trial.path != GpuConvolverRawTrialPath::StagedSync &&
        trial.records.size() != manifest.block_frames)
        return false;
    return true;
}

inline bool merge_records(const GpuConvolverRawTrial& trial,
                          std::map<Identity, Pair>& merged) noexcept {
    for (const auto& record : trial.records) {
        if (!record.valid() || record.generation != trial.generation)
            return false;
        const Identity identity{record.generation, record.sequence};
        auto& pair = merged[identity];
        if (record.kind == SharedIoTraceKind::Terminal) {
            if (pair.terminal != nullptr)
                return false;
            pair.terminal = &record;
        } else if (record.kind == SharedIoTraceKind::Delivery) {
            if (pair.delivery != nullptr)
                return false;
            pair.delivery = &record;
        } else if (record.kind != SharedIoTraceKind::Eligible) {
            return false;
        }
    }
    if (merged.empty())
        return false;
    for (const auto& [identity, pair] : merged) {
        (void)identity;
        if (pair.terminal == nullptr || pair.delivery == nullptr)
            return false;
    }
    return true;
}

inline bool raw_block_ready(GpuConvolverRawTrialPath path,
                            const SharedIoTraceRecord& terminal,
                            const SharedIoTraceRecord& delivery,
                            const GpuConvolverRawManifest& manifest) noexcept {
    if (terminal.generation != delivery.generation || terminal.sequence != delivery.sequence ||
        !terminal.transfer_counters_available || !terminal.cpu_detail_available ||
        !delivery.callback_timing_available)
        return false;
    if (path == GpuConvolverRawTrialPath::SharedAsync &&
        (terminal.transfer_counters.write_buffer_calls != 0 ||
         terminal.transfer_counters.write_buffer_bytes != 0 ||
         terminal.transfer_counters.output_copy_calls != 0 ||
         terminal.transfer_counters.output_copy_bytes != 0 ||
         terminal.transfer_counters.map_async_calls != 0 ||
         terminal.transfer_counters.mapped_readback_memcpy_calls != 0 ||
         terminal.transfer_counters.mapped_readback_memcpy_bytes != 0))
        return false;
    if (path == GpuConvolverRawTrialPath::StagedAsync &&
        terminal.gpu_terminal == SharedIoGpuTerminalDisposition::CompletedAccepted &&
        terminal.transfer_counters.write_buffer_calls == 0 &&
        terminal.transfer_counters.output_copy_calls == 0 &&
        terminal.transfer_counters.map_async_calls == 0 &&
        terminal.transfer_counters.mapped_readback_memcpy_calls == 0)
        return false;
    if (terminal.gpu_terminal == SharedIoGpuTerminalDisposition::None ||
        delivery.delivery == SharedIoDeliveryDisposition::None ||
        delivery.delivery == SharedIoDeliveryDisposition::InvalidRejected ||
        (terminal.gpu_terminal == SharedIoGpuTerminalDisposition::CompletedAccepted &&
         terminal.outcome != SharedIoTraceOutcome::Success))
        return false;

    std::uint64_t callback_cpu = 0;
    if (delivery.callback_end_ns < delivery.callback_start_ns ||
        delivery.result_visible_ns < delivery.callback_end_ns)
        return false;
    if (!duration(terminal, SharedIoTraceStage::WorkerEntry,
                  SharedIoTraceStage::EncodeBegin, callback_cpu))
        return false;
    std::uint64_t ignored = 0;
    if (!duration(terminal, SharedIoTraceStage::EncodeBegin, SharedIoTraceStage::EncodeEnd,
                  ignored) ||
        !duration(terminal, SharedIoTraceStage::SubmitBegin, SharedIoTraceStage::SubmitEnd,
                  ignored) ||
        !duration(terminal, SharedIoTraceStage::SubmitBegin,
                  SharedIoTraceStage::CompletionObserved, ignored) ||
        !duration(terminal, SharedIoTraceStage::CompletionObserved,
                  SharedIoTraceStage::RetirementObserved, ignored))
        return false;
    (void)manifest;
    return true;
}

inline std::string render_block(const GpuConvolverRawManifest& manifest,
                                const GpuConvolverRawTrial& trial, std::size_t ordinal,
                                const raw_writer_detail::Pair& pair) {
    std::ostringstream output;
    const auto& terminal = *pair.terminal;
    const auto& delivery = *pair.delivery;
    const auto observer = std::string_view("worker");
    const auto callback_cpu = delivery.callback_end_ns - delivery.callback_start_ns;
    const auto publish_to_consumable = delivery.result_visible_ns - delivery.callback_end_ns;
    std::uint64_t worker_pack_copy = 0;
    std::uint64_t encode_cpu = 0;
    std::uint64_t submit_cpu = 0;
    std::uint64_t submit_to_completion = 0;
    std::uint64_t completion_to_retirement = 0;
    (void)duration(terminal, SharedIoTraceStage::WorkerEntry, SharedIoTraceStage::EncodeBegin,
                   worker_pack_copy);
    (void)duration(terminal, SharedIoTraceStage::EncodeBegin, SharedIoTraceStage::EncodeEnd,
                   encode_cpu);
    (void)duration(terminal, SharedIoTraceStage::SubmitBegin, SharedIoTraceStage::SubmitEnd,
                   submit_cpu);
    (void)duration(terminal, SharedIoTraceStage::SubmitBegin,
                   SharedIoTraceStage::CompletionObserved, submit_to_completion);
    (void)duration(terminal, SharedIoTraceStage::CompletionObserved,
                   SharedIoTraceStage::RetirementObserved, completion_to_retirement);
    output << R"({"schema":"pulp.gpu-audio.p4.raw.v1","record_kind":"block","trial_id":)"
           << trial.trial_id << R"(,"pair_id":)"
           << (trial.path == GpuConvolverRawTrialPath::StagedSync ? "null"
                                                                   : std::to_string(trial.pair_id))
           << R"(,"path":")" << path_name(trial.path) << R"(","block_ordinal":)" << ordinal
           << R"(,"engine_id":)" << trial.engine_id << R"(,"generation":)" << terminal.generation
           << R"(,"sequence":)" << terminal.sequence << R"(,"gpu_terminal":")"
           << terminal_name(terminal.gpu_terminal) << R"(","delivery":")"
           << delivery_name(delivery.delivery) << R"(","deadline_miss":)"
           << (publish_to_consumable >= manifest.deadline_ns ? "true" : "false")
           << R"(,"watchdog_expiry":)"
           << (worker_pack_copy + encode_cpu + submit_cpu + submit_to_completion +
                       completion_to_retirement >=
                   manifest.watchdog_ns
               ? "true"
               : "false")
           << R"(,"late_completion":)"
           << (terminal.gpu_terminal == SharedIoGpuTerminalDisposition::LateRejected ? "true"
                                                                                         : "false")
           << R"(,"resync_drop":)"
           << ((delivery.delivery_reason == SharedIoFallbackReason::SequenceGap) ? "true"
                                                                                   : "false")
           << R"(,"timings":{)";
    output << R"("callback_cpu":)";
    observation(output, callback_cpu, "audio_callback", "GpuAudioTransport::process");
    output << R"(,"worker_pack_copy":)";
    observation(output, worker_pack_copy, observer, "SharedIoConvolutionSession::pack_input");
    output << R"(,"encode_cpu":)";
    observation(output, encode_cpu, observer, "SharedIoConvolutionSession::encode");
    output << R"(,"submit_cpu":)";
    observation(output, submit_cpu, observer, "SharedIoConvolutionSession::submit");
    output << R"(,"event_processing_cpu":)";
    observation(output, completion_to_retirement, observer,
                "SharedIoConvolutionSession::retirement");
    output << R"(,"retirement_cpu":)";
    observation(output, terminal.retirement_ns, observer,
                "SharedIoConvolutionSession::retirement");
    output << R"(,"worker_other_cpu":)";
    observation(output, terminal.worker_other_ns, observer,
                "SharedIoConvolutionSession::worker_other");
    output << R"(,"worker_end_to_end":)";
    std::uint64_t worker_end_to_end = 0;
    (void)duration(terminal, SharedIoTraceStage::WorkerEntry,
                   SharedIoTraceStage::RetirementObserved, worker_end_to_end);
    observation(output, worker_end_to_end, observer, "SharedIoConvolutionSession::worker");
    output << R"(,"submit_to_completion":)";
    observation(output, submit_to_completion, observer,
                "SharedIoConvolutionSession::completion");
    output << R"(,"gpu_elapsed":)";
    if (terminal.gpu_elapsed_available)
        observation(output, terminal.gpu_elapsed_ns, "gpu", "Dawn::timestamp_query");
    else
        unavailable_observation(output, "gpu", "Dawn::timestamp_query");
    output << R"(,"completion_to_retirement_observed":)";
    observation(output, completion_to_retirement, observer,
                "SharedIoConvolutionSession::retirement");
    output << R"(,"retirement_to_result_visible":)";
    std::uint64_t retirement_to_result = 0;
    if (terminal.has(SharedIoTraceStage::RetirementObserved) &&
        terminal.cpu_ns[static_cast<std::size_t>(SharedIoTraceStage::RetirementObserved)] <=
            delivery.result_visible_ns)
        retirement_to_result =
            delivery.result_visible_ns -
            terminal.cpu_ns[static_cast<std::size_t>(SharedIoTraceStage::RetirementObserved)];
    observation(output, retirement_to_result, observer,
                "GpuAudioTransport::result_visible");
    output << R"(,"publish_to_consumable":)";
    observation(output, publish_to_consumable, "audio_callback",
                "GpuAudioTransport::result_visible");
    output << R"(},"transfers":{)"
           << R"("write_buffer_calls":)" << terminal.transfer_counters.write_buffer_calls
           << R"(,"write_buffer_bytes":)" << terminal.transfer_counters.write_buffer_bytes
           << R"(,"output_copy_calls":)" << terminal.transfer_counters.output_copy_calls
           << R"(,"output_copy_bytes":)" << terminal.transfer_counters.output_copy_bytes
           << R"(,"map_async_calls":)" << terminal.transfer_counters.map_async_calls
           << R"(,"mapped_readback_memcpy_calls":)"
           << terminal.transfer_counters.mapped_readback_memcpy_calls
           << R"(,"mapped_readback_memcpy_bytes":)"
           << terminal.transfer_counters.mapped_readback_memcpy_bytes << "}}";
    return output.str();
}

} // namespace raw_writer_detail

// Emit one complete strict raw campaign. Validation runs before writing any
// bytes, so a rejected campaign cannot leave a prefix that resembles evidence.
inline bool write_gpu_convolver_raw_jsonl(std::ostream& output,
                                          const GpuConvolverRawManifest& manifest,
                                          std::span<const GpuConvolverRawTrial> trials) {
    if (!raw_writer_detail::valid_manifest(manifest) || trials.empty() ||
        trials.size() != manifest.expected_trials)
        return false;

    std::unordered_set<std::uint64_t> trial_ids;
    std::map<std::uint64_t, std::vector<GpuConvolverRawTrialPath>> pair_paths;
    std::size_t staged_sync_count = 0;
    const GpuConvolverRawTrialPath* previous_async = nullptr;
    std::ostringstream body;
    for (const auto& trial : trials) {
        if (!raw_writer_detail::valid_trial(manifest, trial) ||
            !trial_ids.insert(trial.trial_id).second)
            return false;
        if (trial.path == GpuConvolverRawTrialPath::StagedSync)
            ++staged_sync_count;
        else {
            pair_paths[trial.pair_id].push_back(trial.path);
            if (previous_async != nullptr && *previous_async == trial.path)
                return false;
            previous_async = &trial.path;
        }
        std::map<raw_writer_detail::Identity, raw_writer_detail::Pair> trial_records;
        if (!raw_writer_detail::merge_records(trial, trial_records))
            return false;
        std::size_t ordinal = 0;
        std::uint64_t first_sequence = 0;
        bool first = true;
        for (const auto& [identity, pair] : trial_records) {
            if (!raw_writer_detail::raw_block_ready(trial.path, *pair.terminal, *pair.delivery,
                                                    manifest))
                return false;
            if (first) {
                first_sequence = identity.sequence;
                first = false;
            } else if (identity.sequence != first_sequence + ordinal) {
                return false;
            }
            ++ordinal;
        }
        if (trial.path != GpuConvolverRawTrialPath::StagedSync &&
            ordinal != manifest.block_frames)
            return false;
        if (trial.path == GpuConvolverRawTrialPath::StagedAsync &&
            std::none_of(trial_records.begin(), trial_records.end(), [](const auto& item) {
                const auto* terminal = item.second.terminal;
                return terminal->transfer_counters.write_buffer_calls > 0 ||
                       terminal->transfer_counters.output_copy_calls > 0 ||
                       terminal->transfer_counters.map_async_calls > 0 ||
                       terminal->transfer_counters.mapped_readback_memcpy_calls > 0;
            }))
            return false;

        body << R"({"schema":"pulp.gpu-audio.p4.raw.v1","record_kind":"trial_begin","trial_id":)"
             << trial.trial_id << R"(,"pair_id":)"
             << (trial.path == GpuConvolverRawTrialPath::StagedSync ? "null"
                                                                     : std::to_string(trial.pair_id))
             << R"(,"path":")" << raw_writer_detail::path_name(trial.path) << R"(","load":")"
             << raw_writer_detail::load_name(manifest.load) << R"(","block_frames":)"
             << manifest.block_frames << R"(,"sample_rate_hz":)" << manifest.sample_rate_hz
             << R"(,"channels":)" << manifest.channels << R"(,"ir_frames":)" << manifest.ir_frames
             << R"(,"inflight_depth":)" << manifest.inflight_depth << R"(,"lead_blocks":)"
             << manifest.lead_blocks << R"(,"deadline_ns":)" << manifest.deadline_ns
             << R"(,"watchdog_ns":)" << manifest.watchdog_ns << "}\n";
        std::map<std::string, std::uint32_t> terminal_counts{
            {"cancelled_teardown", 0}, {"completed", 0}, {"device_lost", 0},
            {"late_rejected", 0}, {"provider_failure", 0}, {"stale_rejected", 0}};
        std::map<std::string, std::uint32_t> delivery_counts{
            {"cpu_fallback", 0}, {"gpu", 0}, {"passthrough", 0}, {"priming", 0}, {"silence", 0}};
        std::string blocks_digest_input;
        ordinal = 0;
        for (const auto& item : trial_records) {
            const auto block =
                raw_writer_detail::render_block(manifest, trial, ordinal++, item.second);
            body << block << '\n';
            blocks_digest_input += block;
            blocks_digest_input.push_back('\n');
            ++terminal_counts[raw_writer_detail::terminal_name(item.second.terminal->gpu_terminal)];
            ++delivery_counts[raw_writer_detail::delivery_name(item.second.delivery->delivery)];
        }
        body << R"({"schema":"pulp.gpu-audio.p4.raw.v1","record_kind":"trial_end","trial_id":)"
             << trial.trial_id << R"(,"pair_id":)"
             << (trial.path == GpuConvolverRawTrialPath::StagedSync ? "null"
                                                                     : std::to_string(trial.pair_id))
             << R"(,"path":")" << raw_writer_detail::path_name(trial.path)
             << R"(","block_count":)" << ordinal
             << R"(,"blocks_sha256":)";
        raw_writer_detail::json_string(body, pulp::runtime::sha256_hex(blocks_digest_input));
        body << R"(,"gpu_terminal_counts":{)";
        bool first_count = true;
        for (const auto& [name, count] : terminal_counts) {
            if (!first_count)
                body << ',';
            first_count = false;
            raw_writer_detail::json_string(body, name);
            body << ':' << count;
        }
        body << R"(},"delivery_counts":{)";
        first_count = true;
        for (const auto& [name, count] : delivery_counts) {
            if (!first_count)
                body << ',';
            first_count = false;
            raw_writer_detail::json_string(body, name);
            body << ':' << count;
        }
        body << R"(},"device_loss":false,"audio_xrun":false,"driver_stall":false,"ui_frame_p99":)";
        raw_writer_detail::observation(body, trial.ui_frame_p99_ns, "ui", "campaign_ui_observer");
        body << R"(,"duration":)";
        raw_writer_detail::observation(body, trial.duration_ns, "campaign", "campaign_clock");
        body << "}\n";
    }
    if (staged_sync_count != manifest.expected_staged_sync_trials ||
        pair_paths.size() != manifest.expected_matched_pairs)
        return false;
    for (const auto& [pair_id, paths] : pair_paths) {
        (void)pair_id;
        if (paths.size() != 2 ||
            std::count(paths.begin(), paths.end(), GpuConvolverRawTrialPath::StagedAsync) != 1 ||
            std::count(paths.begin(), paths.end(), GpuConvolverRawTrialPath::SharedAsync) != 1)
            return false;
    }

    std::ostringstream manifest_line;
    manifest_line << R"({"schema":"pulp.gpu-audio.p4.raw.v1","record_kind":"manifest",)"
                  << R"("campaign":)";
    raw_writer_detail::json_string(manifest_line, manifest.campaign);
    manifest_line << R"(,"campaign_id":)";
    raw_writer_detail::json_string(manifest_line, manifest.campaign_id);
    manifest_line << R"(,"source_revision":)";
    raw_writer_detail::json_string(manifest_line, manifest.source_revision);
    manifest_line << R"(,"binary_sha256":)";
    raw_writer_detail::json_string(manifest_line, manifest.binary_sha256);
    manifest_line << R"(,"machine_id":)";
    raw_writer_detail::json_string(manifest_line, manifest.machine_id);
    manifest_line << R"(,"machine_model":)";
    raw_writer_detail::json_string(manifest_line, manifest.machine_model);
    manifest_line << R"(,"os_version":)";
    raw_writer_detail::json_string(manifest_line, manifest.os_version);
    manifest_line << R"(,"adapter_name":)";
    raw_writer_detail::json_string(manifest_line, manifest.adapter_name);
    manifest_line << R"(,"adapter_backend":)";
    raw_writer_detail::json_string(manifest_line, manifest.adapter_backend);
    manifest_line << R"(,"adapter_registry_id":)" << manifest.adapter_registry_id
                  << R"(,"adapter_vendor_id":)" << manifest.adapter_vendor_id
                  << R"(,"adapter_device_id":)" << manifest.adapter_device_id << R"(,"provider":)";
    raw_writer_detail::json_string(manifest_line, manifest.provider);
    manifest_line << R"(,"provider_revision":)";
    raw_writer_detail::json_string(manifest_line, manifest.provider_revision);
    manifest_line << R"(,"provider_asset_sha256":)";
    raw_writer_detail::json_string(manifest_line, manifest.provider_asset_sha256);
    manifest_line << R"(,"generated_utc":)";
    raw_writer_detail::json_string(manifest_line, manifest.generated_utc);
    manifest_line << R"(,"build_type":"Release","build_flags":[)";
    for (std::size_t index = 0; index < manifest.build_flags.size(); ++index) {
        if (index != 0)
            manifest_line << ',';
        raw_writer_detail::json_string(manifest_line, manifest.build_flags[index]);
    }
    manifest_line << R"(],"paced":true,"warmup_blocks":)" << manifest.warmup_blocks
                  << R"(,"expected_trials":)" << manifest.expected_trials
                  << R"(,"expected_matched_pairs":)" << manifest.expected_matched_pairs
                  << R"(,"expected_staged_sync_trials":)" << manifest.expected_staged_sync_trials
                  << R"(,"bootstrap_seed":)" << manifest.bootstrap_seed
                  << R"(,"bootstrap_resamples":)" << manifest.bootstrap_resamples
                  << R"(,"row":{"block_frames":)" << manifest.block_frames
                  << R"(,"sample_rate_hz":)" << manifest.sample_rate_hz << R"(,"channels":)"
                  << manifest.channels << R"(,"ir_frames":)" << manifest.ir_frames
                  << R"(,"inflight_depth":)" << manifest.inflight_depth << R"(,"lead_blocks":)"
                  << manifest.lead_blocks << R"(,"deadline_ns":)" << manifest.deadline_ns
                  << R"(,"watchdog_ns":)" << manifest.watchdog_ns << R"(,"load":")"
                  << raw_writer_detail::load_name(manifest.load) << R"("})";
    if (manifest.campaign == "default") {
        manifest_line << R"(,"confirmation_campaign_id":)";
        raw_writer_detail::json_string(manifest_line, manifest.confirmation_campaign_id);
        manifest_line << R"(,"confirmation_summary_sha256":)";
        raw_writer_detail::json_string(manifest_line, manifest.confirmation_summary_sha256);
    }
    manifest_line << R"(,"performance_verdict":"unassigned"})" << '\n';

    output << manifest_line.str() << body.str();
    return static_cast<bool>(output);
}

} // namespace pulp::gpu_audio::detail
