#pragma once

/// @file dawproject_export.hpp
/// Write a Pulp timeline document as DAWproject XML.
///
/// Scope is deliberately BOUNDED and mirrors the importer: flat tracks,
/// beat-anchored clips, inline notes, referenced audio, a single tempo and a
/// single time signature. Everything the document holds beyond that is reported
/// by `plan_export` as a loss and is refused by `run_export` unless the caller
/// consents to each concept by name. This is *bounded DAWproject export*, not
/// Tier-1 support for the format.
///
/// The writer never constructs a plan and never decides what is acceptable to
/// lose. By the time it runs, `run_export` has already enforced per-concept
/// consent, so the writer's whole job is to turn the representable part of the
/// document into bytes and to carry the loss record into the package.
///
/// It emits `project.xml` as text, mirroring the importer's deliberate stop at
/// the XML boundary — packing that entry plus media into a `.dawproject` zip
/// container is the caller's step, so no ZIP writer is pulled into core/.

#include <pulp/interchange/export_plan.hpp>
#include <pulp/timeline/model.hpp>

#include <string>

namespace pulp::dawproject {

struct ExportOptions {
    /// Written into <Application name= version=/>. Identifies the producer to
    /// whichever DAW opens the file.
    std::string application_name = "Pulp";
    std::string application_version = "1.0";
};

/// Build a writer bound to `project`.
///
/// The interchange `ExportWriter` seam receives only an `ExportPlan`, so the
/// document is bound here rather than passed through the plan. `project` must
/// outlive every call to the returned writer.
///
/// Use it through the ordinary contract — there is no one-shot entry point:
///
///     const auto plan = interchange::plan_export(project, interchange::Format::DawProject);
///     interchange::ExportOptions options;
///     options.accepted_losses = plan.required_consent();   // reviewed, not blanket
///     auto artifacts = interchange::run_export(plan, options,
///                                              dawproject::writer(project));
interchange::ExportWriter writer(const timeline::Project& project,
                                 const ExportOptions& options = {});

} // namespace pulp::dawproject
