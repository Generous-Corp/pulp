#pragma once

#include <cstdint>
#include <limits>

namespace pulp::gpu_audio::detail {

// Private measurement vocabulary for the SDK-inspired minimum-lead campaign.
// This is deliberately not part of the installed SDK or the runtime ABI.
struct MinimumLeadProxyAdmission {
    std::uint32_t sample_rate = 0;
    std::uint32_t block_size = 0;
    std::uint32_t requested_lead_blocks = 0;
    std::uint32_t pipeline_depth = 0;
    std::uint32_t provider_slots = 0;
    std::uint32_t max_inflight = 0;
    std::uint32_t batch_size = 0;
    bool provider_identity_authenticated = false;
    bool executable_identity_authenticated = false;
    bool model_identity_authenticated = false;
    bool provider_resources_resident = false;
    bool cpu_fallback_prepared = false;
};

struct MinimumLeadProxyPrediction {
    std::uint32_t version = 0;
    std::uint64_t predicted_ns = 0;
    std::uint64_t observed_ns = 0;
    std::uint64_t deadline_ns = 0;
    bool available = false;
};

struct MinimumLeadProxySample {
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    bool admission_identity_matched = false;
    bool terminal_present = false;
    bool delivery_present = false;
    bool gpu_completed = false;
    bool cpu_fallback_delivered = false;
    bool late = false;
    bool dropped = false;
    bool callback_deadline_missed = false;
    std::uint64_t callback_duration_ns = 0;
    MinimumLeadProxyPrediction prediction;
};

struct MinimumLeadProxyReceipt {
    static constexpr std::uint32_t schema_version = 1;
    static constexpr std::uint64_t unavailable = std::numeric_limits<std::uint64_t>::max();

    std::uint32_t schema = schema_version;
    std::uint32_t evaluated_lead_blocks = 0;
    std::uint64_t sample_count = 0;
    std::uint64_t gpu_completed = 0;
    std::uint64_t cpu_fallback = 0;
    std::uint64_t late_or_dropped = 0;
    std::uint64_t callback_deadline_misses = 0;
    std::uint64_t missing_evidence = 0;
    std::uint64_t prediction_samples = 0;
    std::uint64_t prediction_abs_error_max_ns = unavailable;
    std::uint64_t prediction_margin_min_ns = unavailable;
    bool admission_valid = false;
    bool prediction_valid = false;
    bool complete = false;
    bool accepted = false;
};

// The evaluator is intentionally strict: a one-block result is accepted only
// when the campaign has complete per-sequence evidence, authenticated inputs,
// a prepared fallback, valid predictions, and no fallback/deadline/late/drop
// events. It never infers GPU execution from a ring delivery.
class MinimumLeadProxyEvaluator {
  public:
    static constexpr std::uint64_t minimum_samples = 1000;

    explicit MinimumLeadProxyEvaluator(const MinimumLeadProxyAdmission& admission) noexcept
        : admission_(admission) {}

    void observe(const MinimumLeadProxySample& sample) noexcept {
        ++receipt_.sample_count;
        receipt_.evaluated_lead_blocks = admission_.requested_lead_blocks;
        if (sample.gpu_completed)
            ++receipt_.gpu_completed;
        if (sample.cpu_fallback_delivered)
            ++receipt_.cpu_fallback;
        if (sample.late || sample.dropped)
            ++receipt_.late_or_dropped;
        if (sample.callback_deadline_missed)
            ++receipt_.callback_deadline_misses;
        if (!sample.admission_identity_matched || !sample.terminal_present ||
            !sample.delivery_present || sample.generation == 0 ||
            sample.sequence == std::numeric_limits<std::uint64_t>::max())
            ++receipt_.missing_evidence;

        const auto& prediction = sample.prediction;
        if (prediction.available && prediction.version != 0) {
            ++receipt_.prediction_samples;
            const auto error = prediction.observed_ns >= prediction.predicted_ns
                                   ? prediction.observed_ns - prediction.predicted_ns
                                   : prediction.predicted_ns - prediction.observed_ns;
            if (receipt_.prediction_abs_error_max_ns == MinimumLeadProxyReceipt::unavailable ||
                error > receipt_.prediction_abs_error_max_ns)
                receipt_.prediction_abs_error_max_ns = error;
            if (prediction.deadline_ns >= prediction.observed_ns) {
                const auto margin = prediction.deadline_ns - prediction.observed_ns;
                if (receipt_.prediction_margin_min_ns == MinimumLeadProxyReceipt::unavailable ||
                    margin < receipt_.prediction_margin_min_ns)
                    receipt_.prediction_margin_min_ns = margin;
            } else {
                ++receipt_.missing_evidence;
            }
        } else {
            ++receipt_.missing_evidence;
        }
    }

    MinimumLeadProxyReceipt finish() const noexcept {
        auto out = receipt_;
        out.admission_valid =
            admission_.sample_rate != 0 && admission_.block_size != 0 &&
            admission_.requested_lead_blocks != 0 &&
            admission_.pipeline_depth > admission_.requested_lead_blocks &&
            admission_.provider_slots != 0 && admission_.max_inflight != 0 &&
            admission_.batch_size != 0 && admission_.provider_identity_authenticated &&
            admission_.executable_identity_authenticated &&
            admission_.model_identity_authenticated && admission_.provider_resources_resident &&
            admission_.cpu_fallback_prepared;
        out.prediction_valid = out.prediction_samples == out.sample_count &&
                               out.prediction_samples != 0 &&
                               out.prediction_margin_min_ns != MinimumLeadProxyReceipt::unavailable;
        out.complete = out.sample_count >= minimum_samples && out.missing_evidence == 0;
        out.accepted = out.complete && out.admission_valid && out.prediction_valid &&
                       admission_.requested_lead_blocks == 1 && out.cpu_fallback == 0 &&
                       out.late_or_dropped == 0 && out.callback_deadline_misses == 0 &&
                       out.gpu_completed == out.sample_count;
        return out;
    }

  private:
    MinimumLeadProxyAdmission admission_;
    MinimumLeadProxyReceipt receipt_;
};

} // namespace pulp::gpu_audio::detail
