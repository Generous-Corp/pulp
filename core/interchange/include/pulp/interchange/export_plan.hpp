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
    /// The format declares no writer, or none was supplied.
    NoWriterRegistered,
    /// The writer ran and failed on its own terms.
    WriterFailed,
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
    /// losses it reviewed, so a loss kind introduced later -- by a wider census,
    /// a changed capability row, or a new format version -- stops that pipeline
    /// instead of riding in on consent that was given for something else.
    std::vector<Concept> accepted_losses;
};

/// A document measured against one format's capability table: what survives,
/// and what it costs.
///
/// A plan is only obtainable from plan_export, and run_export only accepts a
/// plan. That is what makes the loss accounting unskippable -- there is no
/// one-shot entry point that writes an artifact without producing the plan
/// first, because no other code can construct one.
class ExportPlan {
  public:
    Format format() const noexcept { return format_; }
    const ConceptCensus& census() const noexcept { return census_; }

    /// Concepts the document uses that the format carries without loss.
    std::span<const Concept> representable() const noexcept { return representable_; }

    const LossManifest& losses() const noexcept { return losses_; }
    bool is_lossless() const noexcept { return losses_.empty(); }

    /// The concepts a caller must accept for run_export to proceed.
    std::vector<Concept> required_consent() const;

  private:
    friend ExportPlan plan_export(const timeline::Project&, Format, const CensusLimits&);
    ExportPlan() = default;

    Format format_{};
    ConceptCensus census_;
    std::vector<Concept> representable_;
    LossManifest losses_;
};

/// Measure a document against a format. Pure: no I/O, no writer, no failure
/// mode -- a census and a table lookup always produce an answer, and an answer
/// that says "everything is lost" is still a plan.
ExportPlan plan_export(const timeline::Project& project, Format format,
                       const CensusLimits& limits = {});

/// Turns a plan into bytes. Writers are supplied per call rather than through a
/// registry, so a test can drive the consent path without global state.
using ExportWriter =
    std::function<runtime::Result<ExportArtifacts, ExportError>(const ExportPlan&)>;

/// Execute a plan. Refuses before touching the writer when the plan loses
/// anything the caller did not accept.
runtime::Result<ExportArtifacts, ExportError>
run_export(const ExportPlan& plan, const ExportOptions& options, const ExportWriter& writer);

} // namespace pulp::interchange
