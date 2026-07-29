#pragma once

#include <pulp/interchange/capability.hpp>
#include <pulp/interchange/census.hpp>
#include <pulp/interchange/concept.hpp>
#include <pulp/interchange/loss.hpp>
#include <pulp/runtime/result.hpp>
#include <pulp/timeline/model.hpp>

#include <cstdint>
#include <functional>
#include <span>
#include <string>
#include <utility>
#include <vector>

namespace pulp::interchange {

/// What one export produced.
struct ExportArtifact {
    std::string name;
    std::vector<std::uint8_t> bytes;
};

struct ExportArtifacts {
    std::vector<ExportArtifact> artifacts;
};

enum class ExportErrorCode : std::uint8_t {
    /// The plan loses concepts the caller did not list in accepted_losses.
    UnacceptedLoss,
    /// No writer was supplied for the format.
    NoWriterRegistered,
    /// The writer ran and failed on its own terms.
    WriterFailed,
    /// The supplied writer belongs to a different interchange format.
    WriterFormatMismatch,
};

struct ExportError {
    ExportErrorCode code = ExportErrorCode::UnacceptedLoss;
    std::string message;
    /// The concepts the error is about. Populated for UnacceptedLoss.
    std::vector<Concept> concepts;
};

struct ExportOptions {
    /// Per-concept consent: every concept the plan reports as lost must appear
    /// here or the export refuses.
    ///
    /// There is deliberately no blanket force flag. A pipeline pins the exact
    /// losses it reviewed, so a newly lossy concept introduced by a wider census
    /// or capability table stops that pipeline instead of riding in on consent
    /// given for something else. Consent identifies concepts, not every semantic
    /// field of a row.
    std::vector<Concept> accepted_losses;
};

/// A document measured against one format's capability table: what survives,
/// and what it costs.
///
/// A plan is only obtainable from plan_export, and run_export only accepts a
/// plan. That is what makes the loss accounting unskippable -- there is no
/// one-shot entry point that writes an artifact without producing the plan
/// first within the consent-gated interchange adapter surface, because no other
/// interchange code can construct one. Strict raw format codecs may remain as
/// separate APIs for callers that want refusal rather than planned loss.
class ExportPlan {
  public:
    Format format() const noexcept { return format_; }
    const timeline::Project& project() const noexcept { return project_; }
    const ConceptCensus& census() const noexcept { return census_; }

    /// Concept kinds present in the document that the format supports in
    /// isolation. This is concept-level capability, not an instance survivor
    /// projection: a supported note inside a dropped container is still listed.
    std::span<const Concept> representable() const noexcept { return representable_; }

    const LossManifest& losses() const noexcept { return losses_; }
    /// True when no model-detectable concept needs a lossy format row.
    /// This is not byte-for-byte or identity-preserving round-trip equivalence.
    bool is_lossless() const noexcept { return losses_.empty(); }

    /// The concepts a caller must accept for run_export to proceed.
    std::vector<Concept> required_consent() const;

  private:
    friend ExportPlan plan_export(const timeline::Project&, Format, const CensusLimits&);
    ExportPlan(timeline::Project project, Format format)
        : format_(format), project_(std::move(project)) {}

    Format format_{};
    timeline::Project project_;
    ConceptCensus census_;
    std::vector<Concept> representable_;
    LossManifest losses_;
};

/// Measure a document against a format. Pure: no I/O, no writer, no failure
/// mode -- a census and a table lookup always produce an answer, and an answer
/// that says "everything is lost" is still a plan.
ExportPlan plan_export(const timeline::Project& project, Format format,
                       const CensusLimits& limits = {});

/// The released v0.759 writer seam. Kept source-compatible so existing SDK
/// consumers can still construct, store, and invoke their std::function.
using ExportWriter =
    std::function<runtime::Result<ExportArtifacts, ExportError>(const ExportPlan&)>;

/// A format-bound byte writer that can only be invoked by run_export.
///
/// Binding the format prevents a plan for one format from authorizing bytes
/// produced by another. Keeping invocation private preserves the plan ->
/// consent -> write sequence mechanically: callers can choose or pass a writer,
/// but cannot execute its callable directly.
class FormatBoundExportWriter {
  public:
    using Function =
        std::function<runtime::Result<ExportArtifacts, ExportError>(const ExportPlan&)>;

    FormatBoundExportWriter() = default;
    FormatBoundExportWriter(Format format, Function function)
        : format_(format), function_(std::move(function)) {}

    explicit operator bool() const noexcept { return static_cast<bool>(function_); }
    Format format() const noexcept { return format_; }

  private:
    friend runtime::Result<ExportArtifacts, ExportError>
    run_export(const ExportPlan&, const ExportOptions&, const FormatBoundExportWriter&);

    Format format_{};
    Function function_;
};

/// Execute a plan. Refuses before touching the writer when the plan loses
/// anything the caller did not accept.
runtime::Result<ExportArtifacts, ExportError>
run_export(const ExportPlan& plan, const ExportOptions& options, const ExportWriter& writer);

/// Execute through a format-bound writer. New adapters should expose this
/// overload so a plan for one format cannot authorize another format's bytes.
runtime::Result<ExportArtifacts, ExportError>
run_export(const ExportPlan& plan, const ExportOptions& options,
           const FormatBoundExportWriter& writer);

} // namespace pulp::interchange
