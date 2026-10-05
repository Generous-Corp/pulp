#pragma once

#include <array>
#include <cstdint>
#include <limits>

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
    bool worker_offline_single_owner = false;
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
    std::uint64_t id_storage_overflow = 0;
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
    // Diagnostic admission still describes the complete campaign cell shape;
    // an external reducer owns the authoritative 100,000-sample acceptance.
    static constexpr std::uint32_t required_slots_mask = 0x1e; // 2,4,8,16
    static constexpr std::uint32_t required_leads_mask = 0x0f; // 1,2,4,8
    static constexpr std::size_t bounded_id_capacity = 131072;
    static constexpr bool uses_bounded_id_storage = true;
    static constexpr std::uint64_t worst_case_probe_budget = bounded_id_capacity * 3;

    // Worker/offline only: this private reducer is intentionally >3 MiB and
    // may probe three bounded tables on every observation. It must be
    // constructed and consumed by one worker, or through an explicit offline
    // synchronized handoff; it is never an audio-callback object.

    explicit MinimumLeadProxyEvaluator(const MinimumLeadProxyAdmission& admission) noexcept
        : admission_(admission) {}

    void observe(const MinimumLeadProxySample& s) noexcept {
        saturating_increment(receipt_.sample_count);
        receipt_.evaluated_lead_blocks = admission_.requested_lead_blocks;
        receipt_.cell_id = admission_.cell_id;
        if (admission_.observer_capacity != 0 &&
            receipt_.sample_count > admission_.observer_capacity)
            saturating_increment(receipt_.observer_overflow);
        if (s.gpu_completed)
            saturating_increment(receipt_.gpu_completed);
        if (s.cpu_fallback_delivered)
            saturating_increment(receipt_.cpu_fallback);
        if (s.late || s.dropped)
            saturating_increment(receipt_.late_or_dropped);

        const bool ids =
            s.admission_id != 0 && s.terminal_id != 0 && s.delivery_id != 0 &&
            s.admission_generation == s.generation && s.terminal_generation == s.generation &&
            s.delivery_generation == s.generation && s.admission_sequence == s.sequence &&
            s.terminal_sequence == s.sequence && s.delivery_sequence == s.sequence &&
            s.admission_epoch == admission_.admission_epoch;
        if (!ids || !s.terminal_present || !s.delivery_present || s.generation == 0 ||
            s.sequence == std::numeric_limits<std::uint64_t>::max())
            saturating_increment(receipt_.missing_evidence);
        const auto admission_result =
            insert_id(admission_ids_, s.admission_id, 0x9e3779b97f4a7c15ULL);
        const auto terminal_result = insert_id(terminal_ids_, s.terminal_id, 0xc2b2ae3d27d4eb4fULL);
        const auto delivery_result = insert_id(delivery_ids_, s.delivery_id, 0x165667b19e3779f9ULL);
        const auto results = {admission_result, terminal_result, delivery_result};
        for (const auto result : results) {
            if (result == IdInsertResult::duplicate)
                saturating_increment(receipt_.duplicate_ids);
            else if (result == IdInsertResult::overflow)
                saturating_increment(receipt_.id_storage_overflow);
        }
        if (!s.identity.raw_hashes_authenticated)
            saturating_increment(receipt_.missing_evidence);
        if (s.identity.provider != admission_.identity.provider ||
            s.identity.executable != admission_.identity.executable ||
            s.identity.model != admission_.identity.model ||
            s.identity.resident_plan != admission_.identity.resident_plan)
            saturating_increment(receipt_.identity_mismatches);
        if (s.generation != 0 && expected_generation_ != 0 && s.generation != expected_generation_)
            saturating_increment(receipt_.generation_mismatches);
        if (s.admission_generation != s.generation || s.terminal_generation != s.generation ||
            s.delivery_generation != s.generation)
            saturating_increment(receipt_.generation_mismatches);
        if (seen_sequence_ && s.sequence != next_sequence_) {
            if (s.sequence < next_sequence_)
                saturating_increment(receipt_.duplicate_records);
            else
                saturating_increment(receipt_.sequence_gaps);
        }
        if (s.generation != 0 && expected_generation_ == 0)
            expected_generation_ = s.generation;
        if (s.sequence != std::numeric_limits<std::uint64_t>::max()) {
            next_sequence_ = s.sequence + 1;
            seen_sequence_ = true;
        }
        if (s.gpu_completed == s.cpu_fallback_delivered)
            saturating_increment(receipt_.missing_evidence);
        if (s.dropped != s.late)
            saturating_increment(receipt_.missing_evidence);
        if (!s.callback_timing_available || s.callback_duration_ns > s.callback_budget_ns)
            saturating_increment(receipt_.callback_deadline_misses);

        const auto& p = s.prediction;
        if (p.available && p.version != 0 && p.provenance != 0) {
            saturating_increment(receipt_.prediction_samples);
            const auto error = p.observed_ns >= p.predicted_ns ? p.observed_ns - p.predicted_ns
                                                               : p.predicted_ns - p.observed_ns;
            if (receipt_.prediction_abs_error_max_ns == Receipt::unavailable ||
                error > receipt_.prediction_abs_error_max_ns)
                receipt_.prediction_abs_error_max_ns = error;
            std::uint64_t allowance = 0;
            std::uint64_t predicted_bound = 0;
            const bool arithmetic_valid = checked_add(p.bound_ns, p.uncertainty_ns, allowance) &&
                                          checked_add(allowance, p.safety_reserve_ns, allowance) &&
                                          checked_add(p.predicted_ns, allowance, predicted_bound);
            if (arithmetic_valid && p.observed_ns > p.predicted_ns &&
                p.observed_ns - p.predicted_ns > allowance)
                saturating_increment(receipt_.predictor_underestimates);
            if (!arithmetic_valid || p.safety_reserve_ns == 0 || predicted_bound > p.deadline_ns ||
                p.deadline_ns < p.observed_ns || p.provenance != admission_.identity.model) {
                saturating_increment(receipt_.missing_evidence);
                saturating_increment(receipt_.invalid_predictions);
            } else {
                const auto margin = p.deadline_ns - p.observed_ns;
                if (receipt_.prediction_margin_min_ns == Receipt::unavailable ||
                    margin < receipt_.prediction_margin_min_ns)
                    receipt_.prediction_margin_min_ns = margin;
            }
        } else {
            saturating_increment(receipt_.missing_evidence);
            saturating_increment(receipt_.invalid_predictions);
        }
    }

    MinimumLeadProxyReceipt finish() const noexcept {
        auto out = receipt_;
        out.admission_valid =
            admission_.sample_rate != 0 && admission_.block_size != 0 &&
            admission_.requested_lead_blocks != 0 &&
            admission_.pipeline_depth > admission_.requested_lead_blocks &&
            admission_.provider_slots != 0 && admission_.max_inflight != 0 && checked_capacity() &&
            admission_.max_inflight <= admission_.capacity && admission_.batch_size != 0 &&
            admission_.batch_size <= admission_.provider_slots && admission_.true_batch_semantics &&
            admission_.provider_resources_resident && admission_.cpu_fallback_prepared &&
            admission_.identity.provider != 0 && admission_.identity.executable != 0 &&
            admission_.identity.model != 0 && admission_.identity.resident_plan != 0 &&
            admission_.identity.raw_hashes_authenticated && admission_.cell_id != 0 &&
            admission_.admission_epoch != 0 &&
            admission_.observer_capacity >= diagnostic_minimum_samples &&
            admission_.slots_mask == required_slots_mask &&
            admission_.leads_mask == required_leads_mask && admission_.cold_runs >= 5 &&
            admission_.steady_runs >= 5 &&
            admission_.validated_cell_denominator >= diagnostic_minimum_samples &&
            admission_.quiet_coverage && admission_.ui_coverage &&
            admission_.gpu_contention_coverage && admission_.overload_coverage &&
            admission_.thermal_coverage && admission_.fused_or_coalesced_dispatch &&
            (admission_.observer_single_owner || admission_.observer_synchronized_handoff) &&
            admission_.worker_offline_single_owner;
        out.prediction_valid = out.prediction_samples == out.sample_count &&
                               out.sample_count != 0 &&
                               out.prediction_margin_min_ns != Receipt::unavailable &&
                               out.predictor_underestimates == 0 && out.invalid_predictions == 0;
        out.complete = out.admission_valid && out.sample_count >= diagnostic_minimum_samples &&
                       out.duplicate_ids == 0 && out.id_storage_overflow == 0 &&
                       out.missing_evidence == 0 && out.duplicate_records == 0 &&
                       out.sequence_gaps == 0 && out.generation_mismatches == 0 &&
                       out.identity_mismatches == 0 && out.observer_overflow == 0 &&
                       out.callback_deadline_misses == 0 && out.gpu_completed == out.sample_count &&
                       out.cpu_fallback == 0;
        // This proxy never owns campaign acceptance. An external reducer must
        // authenticate the complete matrix and may consume this diagnostic receipt.
        out.authoritative_campaign = false;
        out.diagnostic_only = true;
        out.accepted = false;
        return out;
    }

  private:
    using Receipt = MinimumLeadProxyReceipt;
    enum class IdInsertResult : std::uint8_t { inserted, duplicate, overflow };

    MinimumLeadProxyAdmission admission_;
    MinimumLeadProxyReceipt receipt_;
    std::uint64_t expected_generation_ = 0;
    std::uint64_t next_sequence_ = 0;
    bool seen_sequence_ = false;
    std::array<std::uint64_t, bounded_id_capacity> admission_ids_{};
    std::array<std::uint64_t, bounded_id_capacity> terminal_ids_{};
    std::array<std::uint64_t, bounded_id_capacity> delivery_ids_{};

    static bool checked_add(std::uint64_t a, std::uint64_t b, std::uint64_t& out) noexcept {
        if (b > std::numeric_limits<std::uint64_t>::max() - a)
            return false;
        out = a + b;
        return true;
    }

    bool checked_capacity() const noexcept {
        return admission_.requested_lead_blocks <=
                   std::numeric_limits<std::uint32_t>::max() - admission_.provider_slots &&
               admission_.capacity == admission_.provider_slots + admission_.requested_lead_blocks;
    }

    static IdInsertResult insert_id(std::array<std::uint64_t, bounded_id_capacity>& table,
                                    std::uint64_t id, std::uint64_t salt) noexcept {
        if (id == 0)
            return IdInsertResult::duplicate;
        auto hash = id ^ salt;
        hash ^= hash >> 30;
        hash *= 0xbf58476d1ce4e5b9ULL;
        hash ^= hash >> 27;
        hash *= 0x94d049bb133111ebULL;
        hash ^= hash >> 31;
        const auto start = static_cast<std::size_t>(hash % bounded_id_capacity);
        for (std::size_t probe = 0; probe < bounded_id_capacity; ++probe) {
            auto& slot = table[(start + probe) % bounded_id_capacity];
            if (slot == id)
                return IdInsertResult::duplicate;
            if (slot == 0) {
                slot = id;
                return IdInsertResult::inserted;
            }
        }
        return IdInsertResult::overflow;
    }

    static void saturating_increment(std::uint64_t& value) noexcept {
        if (value != std::numeric_limits<std::uint64_t>::max())
            ++value;
    }
};

} // namespace pulp::gpu_audio::detail
