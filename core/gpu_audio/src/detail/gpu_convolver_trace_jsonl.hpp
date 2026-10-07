#pragma once

#include "gpu_convolver_trial_config.hpp"

#include <ostream>
#include <span>

namespace pulp::gpu_audio::detail {

// Diagnostic intermediate format. This is deliberately distinct from the
// strict p4.raw.v1 campaign format: callback/result-visible timings and trial
// digest metadata are unavailable until the benchmark supplies them. The
// writer emits explicit unavailable timing objects and rejects invalid context
// rather than inventing values.
inline bool write_gpu_convolver_trace_jsonl(std::ostream& output,
                                            const GpuConvolverTrialContext& context,
                                            std::span<const SharedIoTraceRecord> records) {
    if (!valid_gpu_convolver_trial_context(context) || records.empty())
        return false;
    // Reject malformed callback timing before writing any receipt prefix.
    for (const auto& record : records) {
        if (!record.valid() || (record.callback_timing_available &&
                                (record.callback_end_ns < record.callback_start_ns ||
                                 record.result_visible_ns < record.callback_end_ns)))
            return false;
    }

    const auto path = context.path == GpuConvolverTrialPath::StagedSync    ? "staged_sync"
                      : context.path == GpuConvolverTrialPath::StagedAsync ? "staged_async"
                                                                           : "shared_async";
    const auto load = [&] {
        switch (context.load) {
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
    }();
    const auto thermal = [&] {
        switch (context.thermal_state) {
        case GpuConvolverThermalState::Nominal:
            return "nominal";
        case GpuConvolverThermalState::Warm:
            return "warm";
        case GpuConvolverThermalState::Throttled:
            return "throttled";
        case GpuConvolverThermalState::Unavailable:
            break;
        }
        return "unavailable";
    }();
    output << R"({"schema":"pulp.gpu-audio.p4.trace.v1","record_kind":"trial_begin","trial_id":)"
           << context.trial_id << R"(,"pair_id":)" << context.pair_id << R"(,"path":")" << path
           << R"(","load":")" << load << R"(","block_frames":)" << context.block_frames
           << R"(,"sample_rate_hz":)" << context.sample_rate_hz << R"(,"channels":)"
           << context.channels << R"(,"ir_frames":)" << context.ir_frames << R"(,"inflight_depth":)"
           << context.inflight_depth << R"(,"queue_capacity":)" << context.queue_capacity
           << R"(,"max_inflight":)" << context.max_inflight << R"(,"lead_blocks":)"
           << context.lead_blocks << R"(,"workgroup_requested":)"
           << (context.workgroup_requested ? "true" : "false") << R"(,"workgroup_joined":)"
           << (context.workgroup_joined ? "true" : "false") << R"(,"thermal_state":")" << thermal
           << R"(","deadline_ns":)" << context.deadline_ns << R"(,"watchdog_ns":)"
           << context.watchdog_ns << R"(,"provenance":{"authenticated":true,"provider_revision":")"
           << context.provider_identity.provider_revision << R"(","adapter_name":")"
           << context.provider_identity.adapter_name << R"(","adapter_backend":")"
           << context.provider_identity.adapter_backend
           << R"(","native_runtime_authenticated":true,"native_runtime_name":")"
           << context.provider_identity.native_runtime_name << R"(","native_runtime_backend":")"
           << context.provider_identity.native_runtime_backend << R"("})" << "}\n";

    for (std::size_t ordinal = 0; ordinal < records.size(); ++ordinal) {
        const auto& record = records[ordinal];
        const auto encode = shared_io_trace_duration(record, SharedIoTraceStage::EncodeBegin,
                                                     SharedIoTraceStage::EncodeEnd);
        const auto submit = shared_io_trace_duration(record, SharedIoTraceStage::SubmitBegin,
                                                     SharedIoTraceStage::SubmitEnd);
        const auto observed = shared_io_trace_duration(record, SharedIoTraceStage::SubmitBegin,
                                                       SharedIoTraceStage::CompletionObserved);
        output << R"({"schema":"pulp.gpu-audio.p4.trace.v1","record_kind":"block","trial_id":)"
               << context.trial_id << R"(,"pair_id":)" << context.pair_id << R"(,"path":")" << path
               << R"(","block_ordinal":)" << ordinal << R"(,"sequence":)" << record.sequence
               << R"(,"gpu_terminal":")" << shared_io_gpu_terminal_name(record.gpu_terminal)
               << R"(","delivery":")" << shared_io_delivery_name(record.delivery)
               << R"(","gpu_reason":")" << shared_io_fallback_reason_name(record.gpu_reason)
               << R"(","delivery_reason":")"
               << shared_io_fallback_reason_name(record.delivery_reason) << R"(","timings":{)"
               << R"("callback_cpu":{"availability":")"
               << (record.callback_timing_available ? "available" : "unavailable") << '"';
        if (record.callback_timing_available && record.callback_end_ns >= record.callback_start_ns)
            output << ",\"value_ns\":" << (record.callback_end_ns - record.callback_start_ns);
        output << R"(},)"
               << R"("encode_cpu":{"availability":")"
               << (encode.available ? "available" : "unavailable") << '"';
        if (encode.available)
            output << ",\"value_ns\":" << encode.ns;
        output << R"(},"submit_cpu":{"availability":")"
               << (submit.available ? "available" : "unavailable") << '"';
        if (submit.available)
            output << ",\"value_ns\":" << submit.ns;
        output << R"(},"submit_to_completion":{"availability":")"
               << (observed.available ? "available" : "unavailable") << '"';
        if (observed.available)
            output << ",\"value_ns\":" << observed.ns;
        output << R"(},"gpu_elapsed":{"availability":)";
        if (record.gpu_elapsed_available)
            output << R"("available","value_ns":)" << record.gpu_elapsed_ns;
        else
            output << R"("unavailable")";
        output << R"(},"publish_to_consumable":{"availability":)";
        if (record.callback_timing_available && record.result_visible_ns >= record.callback_end_ns)
            output << R"("available","value_ns":)"
                   << (record.result_visible_ns - record.callback_end_ns);
        else
            output << R"("unavailable")";
        output << R"(}},)"
                  R"("provenance":{"transfer_counters":"direct","timings":")"
               << (record.callback_timing_available ? "callback_direct" : "worker_direct")
               << R"("}})"
               << "\n";
    }
    return static_cast<bool>(output);
}

} // namespace pulp::gpu_audio::detail
