#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/model.hpp>

#include <cstddef>
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
    LimitExceeded,          // A caller-configurable import resource limit was exceeded.
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

// Hard resource ceilings for one import. Limits are checked before XML parsing
// and before growing importer-owned structural collections. Media byte limits
// are checked immediately after a resolver returns, before inspection, hashing,
// or retention. A zero limit rejects any corresponding non-empty resource.
//
// Resolvers still own the allocation used to produce their returned vector;
// callers handling untrusted packages should apply the same per-call byte
// ceiling while reading package entries.
struct DawProjectImportLimits {
    // Raw project.xml bytes, checked before constructing the XML DOM.
    std::size_t max_xml_bytes = 64u * 1024u * 1024u;
    // Totals across the whole document, not per parent/container.
    std::size_t max_tracks = 16'384;
    std::size_t max_clips = 1'000'000;
    std::size_t max_notes = 5'000'000;
    // Unique retained assets after content-hash deduplication.
    std::size_t max_media_assets = 16'384;
    // All callback invocations, including repeated paths or content.
    std::size_t max_media_resolver_calls = 1'000'000;
    std::size_t max_package_path_bytes = 4'096;
    // Bytes returned by one successful callback.
    std::uint64_t max_media_bytes_per_resolver_call = 2ull * 1024ull * 1024ull * 1024ull;
    // Sum of every successful callback result, including duplicate media.
    std::uint64_t max_total_media_bytes = 16ull * 1024ull * 1024ull * 1024ull;
};

// Parse a DAWproject `project.xml` document and assemble a timeline Project for
// the documented linear subset. Out-of-subset constructs and malformed input are
// rejected with a descriptive error rather than partially imported. Audio
// projects require a resolver so their durable identities can be sealed. The
// two compatibility overloads use DawProjectImportLimits defaults.
runtime::Result<Project, DawProjectImportError> import_dawproject_xml(std::string_view project_xml);
runtime::Result<Project, DawProjectImportError>
import_dawproject_xml(std::string_view project_xml, DawProjectMediaResolver media_resolver);
runtime::Result<Project, DawProjectImportError>
import_dawproject_xml(std::string_view project_xml, DawProjectMediaResolver media_resolver,
                      const DawProjectImportLimits& limits);

} // namespace pulp::timeline
