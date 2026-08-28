#include <pulp_tooling/gpu_health/health_read_result.hpp>

#include <choc/text/choc_JSON.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <set>
#include <string>
#include <utility>

namespace pulp::tooling::gpu_health {
namespace {

template <typename Enum, std::size_t Size>
std::string_view
enum_to_string(Enum value, const std::array<std::pair<Enum, std::string_view>, Size>& entries) {
    for (const auto& [candidate, name] : entries)
        if (candidate == value)
            return name;
    return "unverified";
}

constexpr std::array kMeasurementStatuses{
    std::pair{MeasurementStatus::complete, std::string_view{"complete"}},
    std::pair{MeasurementStatus::incomplete, std::string_view{"incomplete"}},
    std::pair{MeasurementStatus::unavailable, std::string_view{"unavailable"}},
    std::pair{MeasurementStatus::unverified, std::string_view{"unverified"}},
};
constexpr std::array kBudgetStatuses{
    std::pair{BudgetStatus::ratified, std::string_view{"ratified"}},
    std::pair{BudgetStatus::unratified, std::string_view{"unratified"}},
};
constexpr std::array kCacheStates{
    std::pair{CacheState::cold, std::string_view{"cold"}},
    std::pair{CacheState::warm, std::string_view{"warm"}},
};
constexpr std::array kVisibleStates{
    std::pair{VisibleState::prepared, std::string_view{"prepared"}},
    std::pair{VisibleState::fallback, std::string_view{"fallback"}},
    std::pair{VisibleState::unknown, std::string_view{"unknown"}},
};
constexpr std::array kPipelineContributions{
    std::pair{PipelineContribution::material, std::string_view{"material"}},
    std::pair{PipelineContribution::not_material, std::string_view{"not_material"}},
    std::pair{PipelineContribution::unattributed, std::string_view{"unattributed"}},
    std::pair{PipelineContribution::unverified, std::string_view{"unverified"}},
};
constexpr std::array kCausalAttributions{
    std::pair{CausalAttribution::complete, std::string_view{"complete"}},
    std::pair{CausalAttribution::incomplete, std::string_view{"incomplete"}},
    std::pair{CausalAttribution::unavailable, std::string_view{"unavailable"}},
    std::pair{CausalAttribution::unverified, std::string_view{"unverified"}},
};
constexpr std::array kStartupDispositions{
    std::pair{StartupDisposition::queue_b4, std::string_view{"queue-B4"}},
    std::pair{StartupDisposition::queue_b4_investigation,
              std::string_view{"queue-B4-investigation"}},
    std::pair{StartupDisposition::no_change, std::string_view{"no-change"}},
};

struct DiagnosticBinding {
    std::string_view code;
    Verdict verdict;
};

constexpr std::array kDiagnosticBindings{
    DiagnosticBinding{"gpu.startup.pass", Verdict::pass},
    DiagnosticBinding{"gpu.startup.blank", Verdict::fail},
    DiagnosticBinding{"gpu.startup.budget_exceeded", Verdict::fail},
    DiagnosticBinding{"gpu.startup.event_loss", Verdict::unverified},
    DiagnosticBinding{"gpu.startup.timeout", Verdict::unavailable},
    DiagnosticBinding{"gpu.startup.instance_lost", Verdict::unavailable},
    DiagnosticBinding{"gpu.startup.trace_incomplete", Verdict::unverified},
    DiagnosticBinding{"gpu.startup.unavailable", Verdict::unavailable},
    DiagnosticBinding{"gpu.startup.unverified", Verdict::unverified},
};

void set_error(std::string* error, std::string message) {
    if (error != nullptr)
        *error = std::move(message);
}

bool bounded_string(std::string_view value, std::size_t maximum) {
    return !value.empty() && value.size() <= maximum;
}

bool optional_bounded_string(const std::optional<std::string>& value, std::size_t maximum) {
    return !value || bounded_string(*value, maximum);
}

bool sha256(std::string_view value) {
    return value.size() == 64 && std::ranges::all_of(value, [](unsigned char character) {
               return (character >= '0' && character <= '9') ||
                      (character >= 'a' && character <= 'f');
           });
}

bool optional_sha256(const std::optional<std::string>& value) {
    return !value || sha256(*value);
}

bool bounded_duration(const std::optional<double>& value) {
    return !value || (std::isfinite(*value) && *value >= 0.0 && *value <= 300'000.0);
}

std::optional<double> nearest_rank_percentile(std::vector<double> values, double percentile) {
    if (values.empty() || !std::isfinite(percentile) || percentile <= 0.0 || percentile > 100.0)
        return std::nullopt;
    std::ranges::sort(values);
    const auto rank = static_cast<std::size_t>(
        std::ceil(percentile * static_cast<double>(values.size()) / 100.0));
    return values[std::max<std::size_t>(rank, 1) - 1];
}

bool approximately_equal(double left, double right) {
    return std::abs(left - right) <=
           std::max(1.0, std::max(std::abs(left), std::abs(right))) * 1.0e-9;
}

std::optional<Verdict> diagnostic_verdict(std::string_view code) {
    for (const auto& binding : kDiagnosticBindings)
        if (binding.code == code)
            return binding.verdict;
    return std::nullopt;
}

template <typename T>
void set_optional(choc::value::Value& object, const char* key, const std::optional<T>& value) {
    if (value)
        object.setMember(key, *value);
    else
        object.setMember(key, choc::value::Value());
}

std::string compact_json(std::string_view input) {
    std::string output;
    output.reserve(input.size());
    bool in_string = false;
    bool escaped = false;
    for (const char character : input) {
        if (in_string) {
            output.push_back(character);
            if (escaped)
                escaped = false;
            else if (character == '\\')
                escaped = true;
            else if (character == '"')
                in_string = false;
        } else if (character == '"') {
            in_string = true;
            output.push_back(character);
        } else if (character != ' ' && character != '\n' && character != '\r' &&
                   character != '\t') {
            output.push_back(character);
        }
    }
    return output;
}

} // namespace

std::string_view to_string(MeasurementStatus value) {
    return enum_to_string(value, kMeasurementStatuses);
}

std::string_view to_string(BudgetStatus value) {
    return enum_to_string(value, kBudgetStatuses);
}

std::string_view to_string(CacheState value) {
    return enum_to_string(value, kCacheStates);
}

std::string_view to_string(VisibleState value) {
    return enum_to_string(value, kVisibleStates);
}

std::string_view to_string(PipelineContribution value) {
    return enum_to_string(value, kPipelineContributions);
}

std::string_view to_string(CausalAttribution value) {
    return enum_to_string(value, kCausalAttributions);
}

std::string_view to_string(StartupDisposition value) {
    return enum_to_string(value, kStartupDispositions);
}

bool validate(const HealthReadResult& result, std::string* error) {
    const auto fail = [&](std::string message) {
        set_error(error, std::move(message));
        return false;
    };
    if (error != nullptr)
        error->clear();
    if (result.schema != kHealthReadSchema || result.version != kHealthReadVersion)
        return fail("GPU health-read schema identity is unsupported");
    std::string health_error;
    if (!validate(result.health, &health_error))
        return fail("nested GPU health snapshot is invalid: " + health_error);

    const auto& startup = result.startup;
    const auto& budget = startup.budget;
    if (!bounded_string(budget.budget_id, 128) || budget.version != 1 ||
        budget.clock_origin != "editor_open_requested" ||
        budget.endpoint != "first_nonblank_presented_frame" ||
        budget.interaction_hitch_metric != "max_present_interval_before_first_nonblank_ms" ||
        budget.trial_count < 2 || budget.trial_count > kMaximumStartupTrials ||
        budget.cold_trial_count == 0 || budget.warm_trial_count == 0 ||
        budget.cold_trial_count > kMaximumStartupTrials ||
        budget.warm_trial_count > kMaximumStartupTrials ||
        budget.cold_trial_count + budget.warm_trial_count != budget.trial_count ||
        !std::isfinite(budget.percentile) || budget.percentile < 50.0 || budget.percentile > 100.0)
        return fail("startup budget identity, statistic, or trial composition is invalid");
    if (budget.threshold_ms && (!std::isfinite(*budget.threshold_ms) ||
                                *budget.threshold_ms < 0.0 || *budget.threshold_ms > 300'000.0))
        return fail("startup budget threshold is outside the bounded domain");
    if (!optional_bounded_string(budget.threshold_source, 512))
        return fail("startup budget threshold source is invalid");
    if (budget.reference_hosts.size() > kMaximumReferenceHosts)
        return fail("startup budget has too many reference hosts");
    std::set<std::string> reference_hosts;
    for (const auto& host : budget.reference_hosts) {
        if (!bounded_string(host.host_id, 128) || !reference_hosts.insert(host.host_id).second ||
            !std::isfinite(host.refresh_rate_hz) || host.refresh_rate_hz < 1.0 ||
            host.refresh_rate_hz > 1000.0)
            return fail("startup budget reference host is invalid or duplicated");
    }
    if (budget.status == BudgetStatus::ratified) {
        if (!budget.threshold_ms || !budget.threshold_source || budget.reference_hosts.empty())
            return fail("ratified startup budget lacks threshold provenance");
    } else if (budget.threshold_ms || budget.threshold_source || !budget.reference_hosts.empty()) {
        return fail("unratified startup budget must not publish a threshold claim");
    }

    if (!optional_bounded_string(startup.correlation.gpu_evidence_id, 128) ||
        !optional_bounded_string(startup.correlation.trace_evidence_id, 128))
        return fail("startup correlation identity is invalid");
    const auto& capture = startup.capture;
    constexpr std::uint64_t kMaximumExactJsonInteger = 9'007'199'254'740'991ULL;
    if (capture.event_capacity == 0 || capture.event_capacity > 4096 ||
        capture.event_count > capture.event_capacity ||
        capture.dropped_event_count > kMaximumExactJsonInteger ||
        capture.missing_trace_categories.size() > kMaximumMissingTraceCategories)
        return fail("startup capture exceeds its declared bound");
    if (capture.dropped_event_count > 0 && !capture.truncated)
        return fail("dropped startup events must mark the capture truncated");
    std::set<std::string> missing_categories;
    for (const auto& category : capture.missing_trace_categories)
        if (!bounded_string(category, 64) || !missing_categories.insert(category).second)
            return fail("missing trace category is invalid or duplicated");

    const auto& identity = startup.identity;
    if (!bounded_string(identity.pulp_build_id, 128) ||
        !optional_bounded_string(identity.vellum_revision, 128) ||
        !optional_sha256(identity.source_signature_sha256) ||
        !optional_sha256(identity.shader_signature_sha256) ||
        !optional_sha256(identity.expected_target_signature_sha256))
        return fail("startup source or build identity is invalid");
    if (startup.trials.size() > kMaximumStartupTrials)
        return fail("startup trial count exceeds its bound");

    std::vector<double> durations;
    std::vector<double> hitches;
    std::size_t cold_trials = 0;
    std::size_t warm_trials = 0;
    bool blank_failure = false;
    for (std::size_t index = 0; index < startup.trials.size(); ++index) {
        const auto& trial = startup.trials[index];
        if (trial.sequence != index)
            return fail("startup trial sequence must be contiguous from zero");
        if (!bounded_duration(trial.editor_open_to_first_nonblank_ms) ||
            !bounded_duration(trial.interaction_hitch_ms) ||
            !bounded_duration(trial.shader_compile_ms) || !bounded_duration(trial.upload_ms) ||
            !bounded_duration(trial.hidden_frame_ms) || !bounded_duration(trial.present_ms) ||
            (trial.observed_target_signature_sha256 &&
             !sha256(*trial.observed_target_signature_sha256)))
            return fail("startup trial contains an invalid duration or signature");
        const auto bound_verdict = diagnostic_verdict(trial.diagnostic_code);
        if (!bound_verdict || *bound_verdict != trial.verdict)
            return fail("startup diagnostic code disagrees with its verdict");
        if (trial.cache_state == CacheState::cold)
            ++cold_trials;
        else
            ++warm_trials;

        if (trial.verdict == Verdict::pass) {
            if (!trial.editor_open_to_first_nonblank_ms || !trial.interaction_hitch_ms ||
                trial.content_floor_passed != std::optional<bool>{true} ||
                trial.visible_state == VisibleState::unknown || !identity.source_signature_sha256 ||
                !identity.shader_signature_sha256 || !identity.expected_target_signature_sha256 ||
                !trial.observed_target_signature_sha256 ||
                *trial.observed_target_signature_sha256 !=
                    *identity.expected_target_signature_sha256 ||
                identity.adapter_class == AdapterClass::null_adapter ||
                identity.adapter_class == AdapterClass::unknown)
                return fail("passing startup trial lacks nonblank identity proof");
        }
        if (trial.diagnostic_code == "gpu.startup.blank") {
            blank_failure = true;
            const bool signature_mismatch = identity.expected_target_signature_sha256 &&
                                            trial.observed_target_signature_sha256 &&
                                            *identity.expected_target_signature_sha256 !=
                                                *trial.observed_target_signature_sha256;
            if (trial.content_floor_passed != std::optional<bool>{false} && !signature_mismatch)
                return fail("blank startup failure lacks a negative content control");
        }
        if (trial.diagnostic_code == "gpu.startup.budget_exceeded" &&
            (budget.status != BudgetStatus::ratified || !trial.editor_open_to_first_nonblank_ms ||
             *trial.editor_open_to_first_nonblank_ms <= *budget.threshold_ms))
            return fail("budget-exceeded diagnostic lacks a ratified threshold crossing");
        if (trial.diagnostic_code == "gpu.startup.event_loss" && capture.dropped_event_count == 0 &&
            !capture.truncated)
            return fail("event-loss diagnostic lacks dropped or truncated evidence");
        if (trial.diagnostic_code == "gpu.startup.trace_incomplete" &&
            capture.missing_trace_categories.empty())
            return fail("trace-incomplete diagnostic lacks a missing category");
        if (trial.editor_open_to_first_nonblank_ms)
            durations.push_back(*trial.editor_open_to_first_nonblank_ms);
        if (trial.interaction_hitch_ms)
            hitches.push_back(*trial.interaction_hitch_ms);
    }

    const bool complete_capture = !capture.truncated && capture.dropped_event_count == 0 &&
                                  capture.missing_trace_categories.empty() &&
                                  startup.trials.size() == budget.trial_count &&
                                  cold_trials == budget.cold_trial_count &&
                                  warm_trials == budget.warm_trial_count;
    if (startup.status == MeasurementStatus::complete) {
        if (cold_trials != budget.cold_trial_count || warm_trials != budget.warm_trial_count)
            return fail("complete startup measurement violates its cold/warm trial composition");
        if (!complete_capture)
            return fail("complete startup measurement has missing or dropped evidence");
        if (std::ranges::any_of(startup.trials, [](const StartupTrial& trial) {
                return trial.verdict == Verdict::unavailable ||
                       trial.verdict == Verdict::unverified;
            }))
            return fail("complete startup measurement contains an unresolved trial");
    }
    if (startup.status == MeasurementStatus::incomplete && complete_capture)
        return fail("incomplete startup measurement lacks an incompleteness signal");
    if (startup.status == MeasurementStatus::unavailable && !startup.trials.empty())
        return fail("unavailable startup measurement must not contain trials");

    const auto observed = nearest_rank_percentile(durations, budget.percentile);
    const auto hitch = nearest_rank_percentile(hitches, budget.percentile);
    if (startup.observed_percentile_ms.has_value() != observed.has_value() ||
        (observed && !approximately_equal(*startup.observed_percentile_ms, *observed)))
        return fail("startup observed percentile is not derived from its trials");
    if (startup.interaction_hitch_percentile_ms.has_value() != hitch.has_value() ||
        (hitch && !approximately_equal(*startup.interaction_hitch_percentile_ms, *hitch)))
        return fail("startup hitch percentile is not derived from its trials");

    Verdict expected = Verdict::unverified;
    if (startup.status == MeasurementStatus::unavailable) {
        expected = Verdict::unavailable;
    } else if (budget.status == BudgetStatus::ratified) {
        if (blank_failure && startup.status == MeasurementStatus::complete &&
            complete_capture) {
            expected = Verdict::fail;
        } else if (startup.status == MeasurementStatus::complete && observed &&
                   durations.size() == startup.trials.size() &&
                   hitches.size() == startup.trials.size()) {
            expected = *observed <= *budget.threshold_ms ? Verdict::pass : Verdict::fail;
        }
    }
    if (startup.verdict != expected)
        return fail("startup verdict is not supported by the budget and trials");
    if (startup.verdict == Verdict::pass && result.health.verdict != Verdict::pass)
        return fail("startup cannot pass while the nested GPU health check did not pass");

    if (startup.causal_attribution == CausalAttribution::complete &&
        (startup.status != MeasurementStatus::complete || !complete_capture ||
         !startup.correlation.gpu_evidence_id || !startup.correlation.trace_evidence_id))
        return fail("complete causal attribution lacks complete trace evidence");
    if ((startup.pipeline_contribution == PipelineContribution::material ||
         startup.pipeline_contribution == PipelineContribution::not_material) &&
        startup.causal_attribution != CausalAttribution::complete)
        return fail("pipeline attribution claim lacks complete causal evidence");

    if (startup.disposition) {
        if (budget.status != BudgetStatus::ratified ||
            startup.status != MeasurementStatus::complete || !startup.correlation.gpu_evidence_id ||
            !startup.correlation.trace_evidence_id)
            return fail("final startup disposition lacks correlated complete evidence");
        switch (*startup.disposition) {
        case StartupDisposition::queue_b4:
            if (startup.verdict != Verdict::fail ||
                startup.pipeline_contribution != PipelineContribution::material ||
                startup.causal_attribution != CausalAttribution::complete)
                return fail("queue-B4 disposition lacks material causal evidence");
            break;
        case StartupDisposition::queue_b4_investigation:
            if (startup.verdict != Verdict::fail ||
                startup.pipeline_contribution != PipelineContribution::unattributed ||
                startup.causal_attribution == CausalAttribution::complete)
                return fail("queue-B4-investigation disposition is already attributed");
            break;
        case StartupDisposition::no_change:
            if (startup.verdict != Verdict::pass &&
                startup.pipeline_contribution != PipelineContribution::not_material)
                return fail("no-change disposition lacks a passing or non-pipeline result");
            break;
        }
    }
    return true;
}

std::string to_json(const HealthReadResult& result, bool pretty) {
    auto reference_hosts = choc::value::createEmptyArray();
    for (const auto& host : result.startup.budget.reference_hosts) {
        auto value = choc::value::createObject("reference_host");
        value.setMember("host_id", host.host_id);
        value.setMember("refresh_rate_hz", host.refresh_rate_hz);
        reference_hosts.addArrayElement(std::move(value));
    }
    auto budget = choc::value::createObject("budget");
    budget.setMember("budget_id", result.startup.budget.budget_id);
    budget.setMember("version", static_cast<std::int64_t>(result.startup.budget.version));
    budget.setMember("status", std::string(to_string(result.startup.budget.status)));
    budget.setMember("clock_origin", result.startup.budget.clock_origin);
    budget.setMember("endpoint", result.startup.budget.endpoint);
    budget.setMember("interaction_hitch_metric", result.startup.budget.interaction_hitch_metric);
    budget.setMember("trial_count", static_cast<std::int64_t>(result.startup.budget.trial_count));
    budget.setMember("cold_trial_count",
                     static_cast<std::int64_t>(result.startup.budget.cold_trial_count));
    budget.setMember("warm_trial_count",
                     static_cast<std::int64_t>(result.startup.budget.warm_trial_count));
    budget.setMember("percentile", result.startup.budget.percentile);
    set_optional(budget, "threshold_ms", result.startup.budget.threshold_ms);
    set_optional(budget, "threshold_source", result.startup.budget.threshold_source);
    budget.setMember("reference_hosts", std::move(reference_hosts));

    auto correlation = choc::value::createObject("correlation");
    set_optional(correlation, "gpu_evidence_id", result.startup.correlation.gpu_evidence_id);
    set_optional(correlation, "trace_evidence_id", result.startup.correlation.trace_evidence_id);

    auto missing_categories = choc::value::createEmptyArray();
    for (const auto& category : result.startup.capture.missing_trace_categories)
        missing_categories.addArrayElement(category);
    auto capture = choc::value::createObject("capture");
    capture.setMember("event_capacity",
                      static_cast<std::int64_t>(result.startup.capture.event_capacity));
    capture.setMember("event_count", static_cast<std::int64_t>(result.startup.capture.event_count));
    capture.setMember("dropped_event_count",
                      static_cast<std::int64_t>(result.startup.capture.dropped_event_count));
    capture.setMember("truncated", result.startup.capture.truncated);
    capture.setMember("missing_trace_categories", std::move(missing_categories));

    auto identity = choc::value::createObject("identity");
    identity.setMember("pulp_build_id", result.startup.identity.pulp_build_id);
    set_optional(identity, "vellum_revision", result.startup.identity.vellum_revision);
    set_optional(identity, "source_signature_sha256",
                 result.startup.identity.source_signature_sha256);
    set_optional(identity, "shader_signature_sha256",
                 result.startup.identity.shader_signature_sha256);
    set_optional(identity, "expected_target_signature_sha256",
                 result.startup.identity.expected_target_signature_sha256);
    identity.setMember("adapter_class",
                       std::string(to_string(result.startup.identity.adapter_class)));

    auto trials = choc::value::createEmptyArray();
    for (const auto& trial : result.startup.trials) {
        auto value = choc::value::createObject("trial");
        value.setMember("sequence", static_cast<std::int64_t>(trial.sequence));
        value.setMember("cache_state", std::string(to_string(trial.cache_state)));
        set_optional(value, "editor_open_to_first_nonblank_ms",
                     trial.editor_open_to_first_nonblank_ms);
        set_optional(value, "interaction_hitch_ms", trial.interaction_hitch_ms);
        set_optional(value, "shader_compile_ms", trial.shader_compile_ms);
        set_optional(value, "upload_ms", trial.upload_ms);
        set_optional(value, "hidden_frame_ms", trial.hidden_frame_ms);
        set_optional(value, "present_ms", trial.present_ms);
        set_optional(value, "observed_target_signature_sha256",
                     trial.observed_target_signature_sha256);
        set_optional(value, "content_floor_passed", trial.content_floor_passed);
        value.setMember("visible_state", std::string(to_string(trial.visible_state)));
        value.setMember("verdict", std::string(to_string(trial.verdict)));
        value.setMember("diagnostic_code", trial.diagnostic_code);
        trials.addArrayElement(std::move(value));
    }

    auto startup = choc::value::createObject("startup");
    startup.setMember("status", std::string(to_string(result.startup.status)));
    startup.setMember("verdict", std::string(to_string(result.startup.verdict)));
    startup.setMember("budget", std::move(budget));
    startup.setMember("correlation", std::move(correlation));
    startup.setMember("capture", std::move(capture));
    startup.setMember("identity", std::move(identity));
    startup.setMember("trials", std::move(trials));
    set_optional(startup, "observed_percentile_ms", result.startup.observed_percentile_ms);
    set_optional(startup, "interaction_hitch_percentile_ms",
                 result.startup.interaction_hitch_percentile_ms);
    startup.setMember("pipeline_contribution",
                      std::string(to_string(result.startup.pipeline_contribution)));
    startup.setMember("causal_attribution",
                      std::string(to_string(result.startup.causal_attribution)));
    if (result.startup.disposition)
        startup.setMember("disposition", std::string(to_string(*result.startup.disposition)));
    else
        startup.setMember("disposition", choc::value::Value());

    auto root = choc::value::createObject("");
    root.setMember("schema", result.schema);
    root.setMember("version", static_cast<std::int64_t>(result.version));
    root.setMember("health", choc::json::parse(to_json(result.health)));
    root.setMember("startup", std::move(startup));
    const auto json = choc::json::toString(root, pretty);
    return pretty ? json : compact_json(json);
}

} // namespace pulp::tooling::gpu_health
