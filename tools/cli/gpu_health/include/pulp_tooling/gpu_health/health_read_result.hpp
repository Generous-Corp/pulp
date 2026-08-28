#pragma once

#include <pulp_tooling/gpu_health/health_result.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::tooling::gpu_health {

inline constexpr std::string_view kHealthReadSchema = "pulp.gpu-health-read-result.v1";
inline constexpr std::uint32_t kHealthReadVersion = 1;
inline constexpr std::size_t kMaximumStartupTrials = 64;
inline constexpr std::size_t kMaximumReferenceHosts = 8;
inline constexpr std::size_t kMaximumMissingTraceCategories = 16;

enum class MeasurementStatus { complete, incomplete, unavailable, unverified };
enum class BudgetStatus { ratified, unratified };
enum class CacheState { cold, warm };
enum class VisibleState { prepared, fallback, unknown };
enum class PipelineContribution { material, not_material, unattributed, unverified };
enum class CausalAttribution { complete, incomplete, unavailable, unverified };
enum class StartupDisposition { queue_b4, queue_b4_investigation, no_change };

struct ReferenceHost {
    std::string host_id;
    double refresh_rate_hz = 0.0;
};

/// Versioned interpretation contract for startup measurements. An unratified
/// budget fixes the clock, endpoint, statistic, and trial shape without making
/// a performance pass/fail claim before reference-host evidence exists.
struct StartupBudget {
    std::string budget_id;
    std::uint32_t version = 1;
    BudgetStatus status = BudgetStatus::unratified;
    std::string clock_origin{"editor_open_requested"};
    std::string endpoint{"first_nonblank_presented_frame"};
    std::string interaction_hitch_metric{"max_present_interval_before_first_nonblank_ms"};
    std::uint32_t trial_count = 20;
    /// Frozen cache-state controls; both are nonzero and sum to trial_count.
    std::uint32_t cold_trial_count = 10;
    std::uint32_t warm_trial_count = 10;
    double percentile = 95.0;
    std::optional<double> threshold_ms;
    std::optional<std::string> threshold_source;
    std::vector<ReferenceHost> reference_hosts;
};

struct StartupCorrelation {
    std::optional<std::string> gpu_evidence_id;
    std::optional<std::string> trace_evidence_id;
};

struct StartupCapture {
    std::uint32_t event_capacity = 1;
    std::uint32_t event_count = 0;
    std::uint64_t dropped_event_count = 0;
    bool truncated = false;
    std::vector<std::string> missing_trace_categories;
};

struct StartupIdentity {
    std::string pulp_build_id;
    std::optional<std::string> vellum_revision;
    std::optional<std::string> source_signature_sha256;
    std::optional<std::string> shader_signature_sha256;
    std::optional<std::string> expected_target_signature_sha256;
    AdapterClass adapter_class = AdapterClass::unknown;
};

struct StartupTrial {
    std::uint32_t sequence = 0;
    CacheState cache_state = CacheState::cold;
    std::optional<double> editor_open_to_first_nonblank_ms;
    std::optional<double> interaction_hitch_ms;
    std::optional<double> shader_compile_ms;
    std::optional<double> upload_ms;
    std::optional<double> hidden_frame_ms;
    std::optional<double> present_ms;
    std::optional<std::string> observed_target_signature_sha256;
    std::optional<bool> content_floor_passed;
    VisibleState visible_state = VisibleState::unknown;
    Verdict verdict = Verdict::unverified;
    std::string diagnostic_code{"gpu.startup.unverified"};
};

struct StartupMeasurements {
    MeasurementStatus status = MeasurementStatus::unverified;
    Verdict verdict = Verdict::unverified;
    StartupBudget budget;
    StartupCorrelation correlation;
    StartupCapture capture;
    StartupIdentity identity;
    std::vector<StartupTrial> trials;
    std::optional<double> observed_percentile_ms;
    std::optional<double> interaction_hitch_percentile_ms;
    PipelineContribution pipeline_contribution = PipelineContribution::unverified;
    CausalAttribution causal_attribution = CausalAttribution::unverified;
    std::optional<StartupDisposition> disposition;
};

/// Closed response returned by `dev.pulp.gpu/health.read@1`. The health member
/// retains the exact v1 GPU-health contract; startup evidence is bounded and
/// cannot report a performance verdict until its budget and capture are valid.
struct HealthReadResult {
    std::string schema{kHealthReadSchema};
    std::uint32_t version = kHealthReadVersion;
    HealthResult health;
    StartupMeasurements startup;
};

std::string_view to_string(MeasurementStatus value);
std::string_view to_string(BudgetStatus value);
std::string_view to_string(CacheState value);
std::string_view to_string(VisibleState value);
std::string_view to_string(PipelineContribution value);
std::string_view to_string(CausalAttribution value);
std::string_view to_string(StartupDisposition value);

/// Validates bounds and cross-field claims that JSON Schema cannot express,
/// including percentile derivation, capture completeness, signature proof,
/// diagnostic-code bindings, and final disposition evidence.
bool validate(const HealthReadResult& result, std::string* error = nullptr);

std::string to_json(const HealthReadResult& result, bool pretty = false);

} // namespace pulp::tooling::gpu_health
