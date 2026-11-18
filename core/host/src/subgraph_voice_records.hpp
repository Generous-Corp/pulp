#pragma once

// Internal: the data-only check of a document's `voice_declarations`
// section, shared by GraphSerializer::inspect_document and the subgraph
// writer and restore.

#include <pulp/host/graph_serializer.hpp>

#include <choc/containers/choc_Value.h>

#include <string>
#include <string_view>

namespace pulp::host::detail {

/// A declaration identity is non-empty, at most 128 bytes, and holds no
/// control characters.
bool voice_declaration_identity_valid(std::string_view identity) noexcept;

/// Check `root["voice_declarations"]` against `root["subgraphs"]`. Values
/// are data only: template conformance is checked by the subgraph restore.
/// `saved_v7` is false for a v6 or plain document, which cannot carry
/// declarations.
GraphDocumentRefusal validate_voice_declarations(const choc::value::ValueView& root, bool saved_v7,
                                                 std::string& detail);

} // namespace pulp::host::detail
