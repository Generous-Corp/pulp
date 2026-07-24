#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/model.hpp>

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::timeline {

// Import of the DAWproject interchange format (tracks, clips, tempo, meter).
// DAWproject is an open, DAW-neutral XML format; a `.dawproject` file is a ZIP
// whose `project.xml` entry carries the arrangement. This importer consumes the
// XML text and asks a caller-supplied resolver for referenced package media so
// every returned MediaAsset is sealed to the SHA-256 of its actual bytes.
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
// ContentHash is derived from the resolved media bytes; the original path is
// preserved verbatim only as a PackageRelative AssetLocator hint.

enum class DawProjectImportErrorCode : std::uint8_t {
    ParseError,             // The XML text is not well-formed.
    MissingRoot,            // No <Project> document element.
    UnsupportedVersion,     // <Project version> is not a supported major version.
    UnsupportedFeature,     // A construct outside the documented subset (fail closed).
    MissingAttribute,       // A required attribute is absent.
    InvalidValue,           // An attribute is present but unparseable or out of range.
    MissingMediaBytes,      // Referenced package media was not supplied by the resolver.
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

using DawProjectMediaResolver =
    std::function<std::optional<std::vector<std::uint8_t>>(std::string_view package_path)>;

// Parse a DAWproject `project.xml` document and assemble a timeline Project for
// the documented linear subset. Out-of-subset constructs and malformed input are
// rejected with a descriptive error rather than partially imported. Audio
// projects require a resolver so their durable identities can be sealed.
runtime::Result<Project, DawProjectImportError> import_dawproject_xml(std::string_view project_xml);
runtime::Result<Project, DawProjectImportError>
import_dawproject_xml(std::string_view project_xml, DawProjectMediaResolver media_resolver);

} // namespace pulp::timeline
