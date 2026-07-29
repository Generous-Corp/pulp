#pragma once

#include <pulp/interchange/export_plan.hpp>

namespace pulp::smf {

/// Build the Standard MIDI File adapter for the consent-gated export contract.
///
/// The immutable document snapshot comes from the `ExportPlan`; the adapter
/// cannot capture a different project. Pass the returned format-bound writer to
/// `interchange::run_export` with a plan for `interchange::Format::Smf`.
/// Successful execution returns `project.mid`; `run_export` appends the
/// canonical `pulp-loss-manifest.json` artifact after the writer succeeds.
///
/// @return A non-callable Standard MIDI File writer for run_export.
interchange::FormatBoundExportWriter writer();

} // namespace pulp::smf
