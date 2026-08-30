#pragma once

#include <chrono>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace pulp::tooling::gpu_health {
struct HealthReadResult;
}

namespace pulp::inspect {

/// UI-thread producer for the immutable snapshot consumed by
/// `dev.pulp.gpu/health.read@1`. Recording methods fail closed when called from
/// another thread; snapshot() is lock-free, bounded, and safe for a control
/// worker. No method is suitable for an audio callback.
class ControlGpuHealthProvider final {
  public:
    enum class CacheState { cold, warm };
    enum class CacheProvenance {
        unknown,
        fresh_process,
        explicit_cache_reset,
        same_process_editor_reopen,
    };
    enum class MeasurementEndpoint { native_compositor_presentation, headless_capture_complete };

    struct ReferenceHost {
        std::string host_id;
        double refresh_rate_hz = 0.0;
    };

    struct Config {
        std::string pulp_build_id;
        std::string campaign_id{"editor-open"};
        /// Visible Standalone/DAW/Forge adapters must keep the default and
        /// supply an independent compositor timestamp. The constrained
        /// headless adapter selects headless_capture_complete and uses the
        /// capture completion timestamp without claiming presentation.
        MeasurementEndpoint measurement_endpoint =
            MeasurementEndpoint::native_compositor_presentation;
        /// Exact 32-lowercase-hex identifier also emitted by the correlated
        /// A2T trace. Required before a startup trial can claim pass/fail.
        std::optional<std::string> gpu_evidence_id;
        /// Bounded same-instance trace identity emitted beside gpu_evidence_id.
        /// It is deliberately distinct from the GPU evidence identifier and
        /// is the authority every frame observation must match.
        std::optional<std::string> trace_evidence_id;
        std::string budget_id{"pulp.editor-first-visible.v1"};
        std::uint32_t budget_version = 1;
        /// Ratification is accepted only with a positive threshold, source,
        /// and at least one bounded reference host. Invalid input remains
        /// unratified rather than publishing a policy claim.
        bool budget_ratified = false;
        std::optional<double> threshold_ms;
        std::optional<std::string> threshold_source;
        std::vector<ReferenceHost> reference_hosts;
        std::optional<std::string> vellum_revision;
        std::optional<std::string> source_signature_sha256;
        std::optional<std::string> shader_signature_sha256;
        std::optional<std::string> expected_target_signature_sha256;
        std::uint32_t event_capacity = 64;
        std::chrono::milliseconds timeout{5000};
        bool seed_blank_first_frame = false;
    };

    struct AdapterIdentity {
        bool available = false;
        bool native_bridge = false;
        std::string backend;
        std::string name;
        std::string vendor;
        std::string architecture;
    };

    struct FrameObservation {
        /// Unique product editor lifecycle identity. Positive campaign trials
        /// require a fresh bounded value so repeated polls cannot masquerade as
        /// independent cold/warm editor opens.
        std::string lifecycle_id;
        std::optional<CacheState> observed_cache_state;
        /// Producer-observed cache boundary. The provider never derives this
        /// from elapsed time or the requested cold/warm label.
        CacheProvenance cache_provenance = CacheProvenance::unknown;
        AdapterIdentity adapter;
        bool capture_valid = false;
        bool content_floor_passed = false;
        /// Set only by a host producer with direct submission evidence. A
        /// screenshot or available adapter does not prove GPU submission.
        bool gpu_submission_observed = false;
        /// Set only by a native lifecycle producer after the frame became
        /// visible. Back-buffer capture alone is an upper bound, not present.
        bool native_present_observed = false;
        /// Timestamp supplied by the trusted native compositor/presentation
        /// producer. A capture/readback completion timestamp cannot substitute
        /// for this endpoint.
        std::optional<std::chrono::steady_clock::time_point> native_presented_at;
        std::optional<double> interaction_hitch_ms;
        std::optional<double> shader_compile_ms;
        std::optional<double> upload_ms;
        std::optional<double> hidden_frame_ms;
        std::optional<double> present_ms;
        /// Must exactly match Config::trace_evidence_id, the identity emitted
        /// by the provider spans for this campaign. A frame-local identity
        /// cannot establish correlation on its own.
        std::optional<std::string> trace_evidence_id;
        /// Required categories that the exact same-instance trace could not
        /// prove for this observation. These are instrumentation coverage
        /// gaps, not dropped events. Empty is meaningful only with a trace
        /// evidence id.
        std::vector<std::string> missing_trace_categories;
        std::uint64_t non_transparent_pixel_count = 0;
        std::uint64_t distinct_color_count = 0;
        std::string observed_signature_sha256;
        std::chrono::steady_clock::time_point observed_at;
    };

    explicit ControlGpuHealthProvider(Config config);
    ~ControlGpuHealthProvider();
    ControlGpuHealthProvider(const ControlGpuHealthProvider&) = delete;
    ControlGpuHealthProvider& operator=(const ControlGpuHealthProvider&) = delete;

    /// Arms one bounded editor-open trial. Only one trial may be in flight.
    bool begin_editor_open(CacheState cache_state,
                           std::chrono::steady_clock::time_point requested_at) noexcept;
    /// Records bounded raw product evidence. A positive live snapshot is not
    /// by itself an A3 acceptance receipt; the closed A3 verifier independently
    /// binds budget, raw samples, product artifacts, trace, and audio-thread
    /// exclusion evidence.
    bool record_presented_frame(const FrameObservation& frame) noexcept;
    bool record_timeout(std::chrono::steady_clock::time_point observed_at) noexcept;
    bool record_instance_lost() noexcept;
    bool record_dropped_events(std::uint64_t count) noexcept;
    bool awaiting_frame() const noexcept;

    std::shared_ptr<const tooling::gpu_health::HealthReadResult> snapshot() const noexcept;

  private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

} // namespace pulp::inspect
