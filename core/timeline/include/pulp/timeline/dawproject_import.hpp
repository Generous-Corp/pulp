#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/model.hpp>

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::timeline {

/** @addtogroup timeline_interchange
 * @{
 */

/// @file dawproject_import.hpp
/// Bounded import of the documented DAWproject arrangement subset.
///
/// DAWproject is an open, DAW-neutral XML format; a `.dawproject` file is a ZIP
/// whose `project.xml` entry carries the arrangement. This importer consumes the
/// XML text and asks a caller-supplied resolver for referenced package media so
/// every returned MediaAsset is sealed to the SHA-256 of its actual bytes.
///
/// The accepted subset is a major-version-1 Project, one initial tempo and
/// meter, flat tracks, beat-timed arrangement lanes, note clips, and audio
/// clips with package-relative media. Everything outside that subset fails
/// closed rather than being dropped.
///
/// Beats are quarter notes; clip and note positions convert to canonical ticks.
/// Audio paths survive only as PackageRelative locator hints; resolved bytes
/// determine the durable ContentHash.

/// Stable category for a DAWproject import failure.
enum class DawProjectImportErrorCode : std::uint8_t {
    ParseError,             ///< The XML text is not well-formed.
    MissingRoot,            ///< No Project document element exists.
    UnsupportedVersion,     ///< The Project major version is unsupported.
    UnsupportedFeature,     ///< A construct lies outside the accepted subset.
    MissingAttribute,       ///< A required attribute is absent.
    InvalidValue,           ///< An attribute is unparseable or out of range.
    MissingMediaBytes,      ///< The resolver did not supply referenced media.
    DuplicateTrackId,       ///< Two Track elements share an identity.
    DanglingTrackReference, ///< An arrangement lane names an unknown track.
    ModelRejected,          ///< The assembled Project violates a model invariant.
    LimitExceeded,          ///< A configured import resource limit was exceeded.
};

/// Diagnostic returned when an import rejects the whole document.
struct DawProjectImportError {
    /// Failure category.
    DawProjectImportErrorCode code = DawProjectImportErrorCode::ParseError;
    /// Human-readable detail naming the offending element, attribute, or value.
    std::string message;
    /// Underlying model failure when code is ModelRejected.
    ModelError model_error{};
};

/// Resolves a safe package-relative path to complete media bytes.
///
/// The callback runs synchronously on the importing thread and owns its return
/// allocation. Returning null reports missing media. The importer hashes and
/// retains successful bytes only within the configured limits.
using DawProjectMediaResolver =
    std::function<std::optional<std::vector<std::uint8_t>>(std::string_view package_path)>;

namespace detail {
struct DawProjectMediaViewResolver {
    // The returned view is consumed synchronously and is never retained.
    using Function = std::optional<std::span<const std::uint8_t>> (*)(void*, std::string_view);
    void* context = nullptr;
    Function function = nullptr;
    explicit operator bool() const noexcept { return function != nullptr; }
    std::optional<std::span<const std::uint8_t>> operator()(std::string_view path) const {
        return function(context, path);
    }
};
}

/// Hard resource ceilings for one import.
///
/// Limits are checked before XML parsing and before growing importer-owned
/// structural collections. Media byte limits are checked immediately after a
/// resolver returns, before inspection, hashing, or retention. A zero limit
/// rejects any corresponding non-empty resource.
///
/// Resolvers still own the allocation used to produce their returned vector;
/// callers handling untrusted packages should apply the same per-call byte
/// ceiling while reading package entries.
struct DawProjectImportLimits {
    /// Raw project.xml bytes, checked before constructing the XML DOM.
    std::size_t max_xml_bytes = 64u * 1024u * 1024u;
    /// Maximum tracks across the whole document.
    std::size_t max_tracks = 16'384;
    /// Maximum clips across the whole document.
    std::size_t max_clips = 1'000'000;
    /// Maximum notes across the whole document.
    std::size_t max_notes = 5'000'000;
    /// Maximum unique retained assets after content-hash deduplication.
    std::size_t max_media_assets = 16'384;
    /// Maximum resolver calls, including repeated paths or content.
    std::size_t max_media_resolver_calls = 1'000'000;
    /// Maximum UTF-8 bytes in one package-relative path.
    std::size_t max_package_path_bytes = 4'096;
    /// Maximum bytes returned by one successful resolver call.
    std::uint64_t max_media_bytes_per_resolver_call = 2ull * 1024ull * 1024ull * 1024ull;
    /// Maximum bytes returned by all successful calls, including duplicates.
    std::uint64_t max_total_media_bytes = 16ull * 1024ull * 1024ull * 1024ull;
};

/// Parses a DAWproject `project.xml` document without resolving audio media.
///
/// Audio references are rejected because their durable content identities
/// cannot be established. The default resource limits apply.
///
/// @param project_xml Complete project.xml UTF-8 text, borrowed for the call.
/// @return An atomically assembled Project or a structured import error.
runtime::Result<Project, DawProjectImportError> import_dawproject_xml(std::string_view project_xml);

/// Parses a DAWproject `project.xml` document using `media_resolver`.
///
/// The default resource limits apply. The import is atomic: malformed,
/// unsupported, unresolved, or over-limit input returns an error and no partial
/// Project.
///
/// @param project_xml Complete project.xml UTF-8 text, borrowed for the call.
/// @param media_resolver Synchronous resolver for package-relative media paths.
/// @return An atomically assembled Project or a structured import error.
runtime::Result<Project, DawProjectImportError>
import_dawproject_xml(std::string_view project_xml, DawProjectMediaResolver media_resolver);

/// Parses a DAWproject `project.xml` document under explicit resource limits.
///
/// Out-of-subset constructs and malformed input are rejected rather than
/// partially imported. Audio projects require a resolver so their durable
/// identities can be sealed. The resolver runs synchronously and may be invoked
/// repeatedly for the same path.
///
/// @param project_xml Complete project.xml UTF-8 text, borrowed for the call.
/// @param media_resolver Synchronous resolver for package-relative media paths.
/// @param limits Hard ceilings applied throughout parsing and assembly.
/// @return An atomically assembled Project or a structured import error.
runtime::Result<Project, DawProjectImportError>
import_dawproject_xml(std::string_view project_xml, DawProjectMediaResolver media_resolver,
                      const DawProjectImportLimits& limits);

namespace detail {
runtime::Result<Project, DawProjectImportError>
import_dawproject_xml_view(std::string_view project_xml,
                           DawProjectMediaViewResolver media_resolver,
                           const DawProjectImportLimits& limits);
}

/// @}

} // namespace pulp::timeline
