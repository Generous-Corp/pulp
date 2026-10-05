#pragma once

#include <cstdint>
#include <limits>
#include <unordered_set>

namespace pulp::gpu_audio::detail {

// Private, default-off measurement vocabulary. This is not installed and does
// not participate in the runtime or SDK ABI.
struct MinimumLeadProxyIdentity {
    std::uint64_t provider = 0;
    std::uint64_t executable = 0;
    std::uint64_t model = 0;
    std::uint64_t resident_plan = 0;
    bool raw_hashes_authenticated = false;
};

struct MinimumLeadProxyAdmission {
    std::uint32_t sample_rate = 0;
    std::uint32_t block_size = 0;
    std::uint32_t requested_lead_blocks = 0;
    std::uint32_t pipeline_depth = 0;
    std::uint32_t provider_slots = 0;
    std::uint32_t max_inflight = 0;
    std::uint32_t capacity = 0;
    std::uint32_t batch_size = 0;
    bool true_batch_semantics = false;
    bool provider_resources_resident = false;
    bool cpu_fallback_prepared = false;
    MinimumLeadProxyIdentity identity{};
    std::uint64_t cell_id = 0;
    std::uint32_t contention_level = 0;
    std::uint32_t thermal_level = 0;
    std::uint32_t observer_capacity = 0;
    std::uint64_t admission_epoch = 0;
    std::uint32_t slots_mask = 0;
    std::uint32_t leads_mask = 0;
    std::uint32_t cold_runs = 0;
    std::uint32_t steady_runs = 0;
    std::uint64_t validated_cell_denominator = 0;
    bool quiet_coverage = false;
    bool ui_coverage = false;
    bool gpu_contention_coverage = false;
    bool overload_coverage = false;
    bool thermal_coverage = false;
    bool fused_or_coalesced_dispatch = false;
    bool observer_single_owner = false;
    bool observer_synchronized_handoff = false;
};

struct MinimumLeadProxyPrediction {
    std::uint32_t version = 0;
    std::uint64_t provenance = 0;
    std::uint64_t predicted_ns = 0;
    std::uint64_t observed_ns = 0;
    std::uint64_t bound_ns = 0;
    std::uint64_t uncertainty_ns = 0;
    std::uint64_t safety_reserve_ns = 0;
    std::uint64_t deadline_ns = 0;
    bool available = false;
};

struct MinimumLeadProxySample {
    std::uint64_t generation = 0;
    std::uint64_t sequence = 0;
    std::uint64_t admission_id = 0;
    std::uint64_t terminal_id = 0;
    std::uint64_t delivery_id = 0;
    std::uint64_t admission_generation = 0;
    std::uint64_t admission_sequence = 0;
    std::uint64_t terminal_generation = 0;
    std::uint64_t terminal_sequence = 0;
    std::uint64_t delivery_generation = 0;
    std::uint64_t delivery_sequence = 0;
    std::uint64_t admission_epoch = 0;
    MinimumLeadProxyIdentity identity{};
    bool terminal_present = false;
    bool delivery_present = false;
    bool gpu_completed = false;
    bool cpu_fallback_delivered = false;
    bool late = false;
    bool dropped = false;
    bool callback_timing_available = false;
    std::uint64_t callback_duration_ns = 0;
    std::uint64_t callback_budget_ns = 0;
    MinimumLeadProxyPrediction prediction{};
};

struct MinimumLeadProxyReceipt {
    static constexpr std::uint32_t schema_version = 2;
    static constexpr std::uint64_t unavailable = std::numeric_limits<std::uint64_t>::max();

    std::uint32_t schema = schema_version;
    std::uint32_t evaluated_lead_blocks = 0;
    std::uint64_t cell_id = 0;
    std::uint64_t sample_count = 0;
    std::uint64_t gpu_completed = 0;
    std::uint64_t cpu_fallback = 0;
    std::uint64_t late_or_dropped = 0;
    std::uint64_t callback_deadline_misses = 0;
    std::uint64_t missing_evidence = 0;
    std::uint64_t duplicate_records = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t generation_mismatches = 0;
    std::uint64_t identity_mismatches = 0;
    std::uint64_t predictor_underestimates = 0;
    std::uint64_t invalid_predictions = 0;
    std::uint64_t observer_overflow = 0;
    std::uint64_t duplicate_ids = 0;
    std::uint64_t prediction_samples = 0;
    std::uint64_t prediction_abs_error_max_ns = unavailable;
    std::uint64_t prediction_margin_min_ns = unavailable;
    bool admission_valid = false;
    bool prediction_valid = false;
    bool complete = false;
    bool diagnostic_only = true;
    bool authoritative_campaign = false;
    bool accepted = false;
};

class MinimumLeadProxyEvaluator {
  public:
    static constexpr std::uint64_t diagnostic_minimum_samples = 1000;
    static constexpr std::uint64_t authoritative_minimum_samples = 100000;

    explicit MinimumLeadProxyEvaluator(const MinimumLeadProxyAdmission& admission) noexcept
        : admission_(admission) {}

    void observe(const MinimumLeadProxySample& s) noexcept {
        ++receipt_.sample_count;
        receipt_.evaluated_lead_blocks = admission_.requested_lead_blocks;
        receipt_.cell_id = admission_.cell_id;
        if (admission_.observer_capacity != 0 &&
            receipt_.sample_count > admission_.observer_capacity)
            ++receipt_.observer_overflow;
        if (s.gpu_completed)
            ++receipt_.gpu_completed;
        if (s.cpu_fallback_delivered)
            ++receipt_.cpu_fallback;
        if (s.late || s.dropped)
            ++receipt_.late_or_dropped;

        const bool ids =
            s.admission_id != 0 && s.terminal_id != 0 && s.delivery_id != 0 &&
            s.admission_generation == s.generation && s.terminal_generation == s.generation &&
            s.delivery_generation == s.generation && s.admission_sequence == s.sequence &&
            s.terminal_sequence == s.sequence && s.delivery_sequence == s.sequence &&
            s.admission_epoch == admission_.admission_epoch;
        if (!ids || !s.terminal_present || !s.delivery_present || s.generation == 0)
            ++receipt_.missing_evidence;
        if (!admission_ids_.insert(s.admission_id).second ||
            !terminal_ids_.insert(s.terminal_id).second ||
            !delivery_ids_.insert(s.delivery_id).second)
            saturating_increment(receipt_.duplicate_ids);
        if (s.identity.provider != admission_.identity.provider ||
            s.identity.executable != admission_.identity.executable ||
            s.identity.model != admission_.identity.model ||
            s.identity.resident_plan != admission_.identity.resident_plan)
            ++receipt_.identity_mismatches;
        if (s.generation != 0 && expected_generation_ != 0 && s.generation != expected_generation_)
            ++receipt_.generation_mismatches;
        if (s.admission_generation != s.generation || s.terminal_generation != s.generation ||
            s.delivery_generation != s.generation)
            ++receipt_.generation_mismatches;
        if (seen_sequence_ && s.sequence != next_sequence_) {
            if (s.sequence < next_sequence_)
                ++receipt_.duplicate_records;
            else
                ++receipt_.sequence_gaps;
        }
        if (s.generation != 0 && expected_generation_ == 0)
            expected_generation_ = s.generation;
        if (s.sequence != std::numeric_limits<std::uint64_t>::max()) {
            next_sequence_ = s.sequence + 1;
            seen_sequence_ = true;
        }
        if (s.gpu_completed == s.cpu_fallback_delivered)
            ++receipt_.missing_evidence;
        if (s.dropped != s.late)
            ++receipt_.missing_evidence;
        if (!s.callback_timing_available || s.callback_duration_ns > s.callback_budget_ns)
            ++receipt_.callback_deadline_misses;

        const auto& p = s.prediction;
        if (p.available && p.version != 0 && p.provenance != 0) {
            ++receipt_.prediction_samples;
            const auto error = p.observed_ns >= p.predicted_ns ? p.observed_ns - p.predicted_ns
                                                               : p.predicted_ns - p.observed_ns;
            if (receipt_.prediction_abs_error_max_ns == Receipt::unavailable ||
                error > receipt_.prediction_abs_error_max_ns)
                receipt_.prediction_abs_error_max_ns = error;
            const auto allowance = p.bound_ns + p.uncertainty_ns + p.safety_reserve_ns;
            if (p.observed_ns > p.predicted_ns && p.observed_ns - p.predicted_ns > allowance)
                ++receipt_.predictor_underestimates;
            if (p.safety_reserve_ns == 0 || p.predicted_ns + allowance > p.deadline_ns ||
                p.deadline_ns < p.observed_ns || p.provenance != admission_.identity.model) {
                ++receipt_.missing_evidence;
                ++receipt_.invalid_predictions;
            } else {
                const auto margin = p.deadline_ns - p.observed_ns;
                if (receipt_.prediction_margin_min_ns == Receipt::unavailable ||
                    margin < receipt_.prediction_margin_min_ns)
                    receipt_.prediction_margin_min_ns = margin;
            }
        } else {
            ++receipt_.missing_evidence;
            ++receipt_.invalid_predictions;
        }
    }

    MinimumLeadProxyReceipt finish() const noexcept {
        auto out = receipt_;
        out.admission_valid =
            admission_.sample_rate != 0 && admission_.block_size != 0 &&
            admission_.requested_lead_blocks != 0 &&
            admission_.pipeline_depth > admission_.requested_lead_blocks &&
            admission_.provider_slots != 0 && admission_.max_inflight != 0 &&
            admission_.capacity == admission_.provider_slots + admission_.requested_lead_blocks &&
            admission_.max_inflight <= admission_.capacity && admission_.batch_size != 0 &&
            admission_.batch_size <= admission_.provider_slots && admission_.true_batch_semantics &&
            admission_.provider_resources_resident && admission_.cpu_fallback_prepared &&
            admission_.identity.provider != 0 && admission_.identity.executable != 0 &&
            admission_.identity.model != 0 && admission_.identity.resident_plan != 0 &&
            admission_.identity.raw_hashes_authenticated && admission_.cell_id != 0 &&
            admission_.admission_epoch != 0 &&
            admission_.observer_capacity >= diagnostic_minimum_samples &&
            admission_.fused_or_coalesced_dispatch &&
            (admission_.observer_single_owner || admission_.observer_synchronized_handoff);
        out.prediction_valid = out.prediction_samples == out.sample_count &&
                               out.sample_count != 0 &&
                               out.prediction_margin_min_ns != Receipt::unavailable &&
                               out.predictor_underestimates == 0 && out.invalid_predictions == 0;
        out.complete = out.sample_count >= diagnostic_minimum_samples && out.duplicate_ids == 0 &&
                       out.missing_evidence == 0 && out.duplicate_records == 0 &&
                       out.sequence_gaps == 0 && out.generation_mismatches == 0 &&
                       out.identity_mismatches == 0 && out.observer_overflow == 0 &&
                       out.callback_deadline_misses == 0 && out.gpu_completed == out.sample_count &&
                       out.cpu_fallback == 0;
        out.authoritative_campaign = out.sample_count >= authoritative_minimum_samples &&
                                     admission_.slots_mask == 0x1e &&
                                     admission_.leads_mask == 0x0f && admission_.cold_runs == 5 &&
                                     admission_.steady_runs == 5 &&
                                     admission_.validated_cell_denominator == out.sample_count &&
                                     admission_.quiet_coverage && admission_.ui_coverage &&
                                     admission_.gpu_contention_coverage &&
                                     admission_.overload_coverage && admission_.thermal_coverage;
        out.diagnostic_only = !out.authoritative_campaign;
        out.accepted = out.authoritative_campaign && out.complete && out.admission_valid &&
                       out.prediction_valid && admission_.requested_lead_blocks == 1 &&
                       out.late_or_dropped == 0;
        return out;
    }

  private:
    using Receipt = MinimumLeadProxyReceipt;
    MinimumLeadProxyAdmission admission_;
    MinimumLeadProxyReceipt receipt_;
    std::uint64_t expected_generation_ = 0;
    std::uint64_t next_sequence_ = 0;
    bool seen_sequence_ = false;
    std::unordered_set<std::uint64_t> admission_ids_;
    std::unordered_set<std::uint64_t> terminal_ids_;
    std::unordered_set<std::uint64_t> delivery_ids_;

    static void saturating_increment(std::uint64_t& value) noexcept {
        if (value != std::numeric_limits<std::uint64_t>::max())
            ++value;
    }
};

} // namespace pulp::gpu_audio::detail
