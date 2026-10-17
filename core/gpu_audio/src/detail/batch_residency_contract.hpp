#pragma once

#include <cstdint>
#include <limits>

namespace pulp::gpu_audio::detail {

// Private, provider-neutral G1 evidence.  This is deliberately not installed:
// it describes what a provider must prove before a width>1 dispatch can be
// called batched and resident, without adding a runtime or SDK ABI.
struct BatchResidencyAdmission {
    std::uint32_t sample_rate = 0;
    std::uint32_t block_size = 0;
    std::uint32_t batch_width = 0;
    std::uint32_t provider_slots = 0;
    std::uint64_t resident_bytes = 0;
    std::uint64_t provider_id = 0;
    std::uint64_t executable_id = 0;
    std::uint64_t model_id = 0;
    std::uint64_t resident_plan_id = 0;
    std::uint64_t generation = 0;
    bool authenticated = false;
    bool true_batch = false;
    bool resources_resident = false;
    bool cpu_fallback_prepared = false;
};

struct BatchResidencySample {
    std::uint64_t sequence = 0;
    std::uint64_t dispatch_id = 0;
    std::uint32_t batch_width = 0;
    std::uint64_t resident_bytes = 0;
    std::uint64_t provider_id = 0;
    std::uint64_t executable_id = 0;
    std::uint64_t model_id = 0;
    std::uint64_t resident_plan_id = 0;
    std::uint64_t generation = 0;
    bool batched_dispatch = false;
    bool resources_resident = false;
    bool gpu_completed = false;
};

struct BatchResidencyReceipt {
    static constexpr std::uint64_t minimum_samples = 8;
    static constexpr std::uint64_t unavailable = std::numeric_limits<std::uint64_t>::max();

    std::uint64_t sample_count = 0;
    std::uint64_t gpu_completed = 0;
    std::uint64_t invalid_samples = 0;
    std::uint64_t missing_dispatch_ids = 0;
    std::uint64_t duplicate_dispatch_ids = 0;
    std::uint64_t sequence_gaps = 0;
    std::uint64_t identity_mismatches = 0;
    std::uint64_t width_mismatches = 0;
    std::uint64_t residency_mismatches = 0;
    bool admission_valid = false;
    bool complete = false;
    bool diagnostic_only = true;
    bool accepted = false;
};

class BatchResidencyEvaluator final {
  public:
    explicit BatchResidencyEvaluator(BatchResidencyAdmission admission) noexcept
        : admission_(admission) {}

    void observe(const BatchResidencySample& sample) noexcept {
        ++receipt_.sample_count;
        if (sample.gpu_completed)
            ++receipt_.gpu_completed;
        if (sample.dispatch_id == 0)
            ++receipt_.missing_dispatch_ids;
        if (seen_sequence_ && sample.sequence != next_sequence_) {
            if (sample.sequence < next_sequence_)
                ++receipt_.duplicate_dispatch_ids;
            else
                ++receipt_.sequence_gaps;
        }
        if (sample.sequence != std::numeric_limits<std::uint64_t>::max()) {
            next_sequence_ = sample.sequence + 1;
            seen_sequence_ = true;
        }
        if (sample.generation != admission_.generation)
            ++receipt_.identity_mismatches;
        if (sample.provider_id != admission_.provider_id ||
            sample.executable_id != admission_.executable_id ||
            sample.model_id != admission_.model_id ||
            sample.resident_plan_id != admission_.resident_plan_id)
            ++receipt_.identity_mismatches;
        if (sample.batch_width != admission_.batch_width || !sample.batched_dispatch)
            ++receipt_.width_mismatches;
        if (sample.resident_bytes != admission_.resident_bytes || !sample.resources_resident)
            ++receipt_.residency_mismatches;
        if (sample.dispatch_id == 0 || !sample.gpu_completed)
            ++receipt_.invalid_samples;
    }

    BatchResidencyReceipt finish() const noexcept {
        auto out = receipt_;
        out.admission_valid = admission_.sample_rate != 0 && admission_.block_size != 0 &&
                              admission_.batch_width > 1 &&
                              admission_.batch_width <= admission_.provider_slots &&
                              admission_.resident_bytes != 0 && admission_.provider_id != 0 &&
                              admission_.executable_id != 0 && admission_.model_id != 0 &&
                              admission_.resident_plan_id != 0 && admission_.generation != 0 &&
                              admission_.authenticated && admission_.true_batch &&
                              admission_.resources_resident && admission_.cpu_fallback_prepared;
        out.complete = out.admission_valid &&
                       out.sample_count >= BatchResidencyReceipt::minimum_samples &&
                       out.gpu_completed == out.sample_count && out.invalid_samples == 0 &&
                       out.missing_dispatch_ids == 0 && out.duplicate_dispatch_ids == 0 &&
                       out.sequence_gaps == 0 && out.identity_mismatches == 0 &&
                       out.width_mismatches == 0 && out.residency_mismatches == 0;
        return out;
    }

  private:
    BatchResidencyAdmission admission_;
    BatchResidencyReceipt receipt_;
    std::uint64_t next_sequence_ = 0;
    bool seen_sequence_ = false;
};

} // namespace pulp::gpu_audio::detail
