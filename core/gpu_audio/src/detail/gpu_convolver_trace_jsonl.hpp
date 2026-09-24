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

    const auto path = context.path == GpuConvolverTrialPath::StagedSync
                          ? "staged_sync"
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
    output << R"({"schema":"pulp.gpu-audio.p4.trace.v1","record_kind":"trial_begin","trial_id":)"
           << context.trial_id << R"(,"pair_id":)" << context.pair_id << R"(,"path":")" << path
           << R"(","load":")" << load << R"(","block_frames":)" << context.block_frames
           << R"(,"sample_rate_hz":)" << context.sample_rate_hz << R"(,"channels":)"
           << context.channels << R"(,"ir_frames":)" << context.ir_frames << R"(,"inflight_depth":)"
           << context.inflight_depth << R"(,"lead_blocks":)" << context.lead_blocks
           << R"(,"deadline_ns":)" << context.deadline_ns << R"(,"watchdog_ns":)"
           << context.watchdog_ns << "}\n";

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
               << R"(","timings":{)"
               << R"("callback_cpu":{"availability":"unavailable"},)"
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
        output << R"(},"gpu_elapsed":{"availability":"unavailable"},)"
                  R"("publish_to_consumable":{"availability":"unavailable"}},)"
                  R"("provenance":{"transfer_counters":"direct","timings":"worker_direct"}})"
               << "\n";
    }
    return static_cast<bool>(output);
}

} // namespace pulp::gpu_audio::detail
