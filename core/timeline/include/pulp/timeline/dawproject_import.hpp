#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/model.hpp>

#include <cstdint>
#include <string>
#include <string_view>

namespace pulp::timeline {

// Import of the DAWproject interchange format (tracks, clips, tempo, meter).
// DAWproject is an open, DAW-neutral XML format; a `.dawproject` file is a ZIP
// whose `project.xml` entry carries the arrangement. This importer consumes that
// XML text directly. Unzipping the container and the CLI import verb are
// deliberate follow-ups — this module is the linear, in-memory foundation.
//
// Documented subset (everything outside it fails closed, never a silent drop):
//   * <Project version="1.x">                       — root, major version 1 only
//   * <Transport><Tempo unit="bpm" value=.../>       — single tempo at bar 1
//   * <Transport><TimeSignature numerator/denominator/> — single meter at bar 1
//   * <Structure> flat <Track> elements              — no nested group tracks
//   * <Arrangement><Lanes timeUnit="beats">          — musical (beats) timing only
//       <Lanes track="<track-id>"><Clips><Clip time duration>
//         <Notes><Note .../></Notes>  |  <Audio sampleRate duration><File path/>
//
// Beats are quarter notes; clip/note positions convert to canonical ticks via
// timebase::kTicksPerQuarter. Audio clips reference a MediaAsset whose durable
// ContentHash is provisionally derived from the referenced package path (the
// bytes are not available until the container is unzipped — a follow-up); the
// original path is preserved verbatim as a PackageRelative AssetLocator hint.

enum class DawProjectImportErrorCode : std::uint8_t {
    ParseError,             // The XML text is not well-formed.
    MissingRoot,            // No <Project> document element.
    UnsupportedVersion,     // <Project version> is not a supported major version.
    UnsupportedFeature,     // A construct outside the documented subset (fail closed).
    MissingAttribute,       // A required attribute is absent.
    InvalidValue,           // An attribute is present but unparseable or out of range.
    DuplicateTrackId,       // Two <Track> elements share an id.
    DanglingTrackReference, // A <Lanes track="..."> references an unknown track id.
    ModelRejected,          // The timeline model rejected the assembled project.
};

struct DawProjectImportError {
    DawProjectImportErrorCode code = DawProjectImportErrorCode::ParseError;
    // Human-readable detail naming the offending element/attribute/value.
    std::string message;
    // Populated only when code == ModelRejected: the underlying model failure.
    ModelError model_error{};
};

// Parse a DAWproject `project.xml` document and assemble a timeline Project for
// the documented linear subset. Out-of-subset constructs and malformed input are
// rejected with a descriptive error rather than partially imported.
runtime::Result<Project, DawProjectImportError>
import_dawproject_xml(std::string_view project_xml);

} // namespace pulp::timeline
