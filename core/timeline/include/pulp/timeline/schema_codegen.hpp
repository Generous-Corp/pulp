#pragma once

#include <pulp/runtime/result.hpp>
#include <pulp/timeline/schema_json.hpp>
#include <pulp/timeline/schema_registry.hpp>

#include <cstddef>
#include <string>

namespace pulp::timeline {

// Deterministic projection of a SchemaRegistry into a single JSON-Schema
// document. Every registered TypeSchema becomes a `$defs` entry carrying its
// domain, current version, fields, required set, and migration edges, so the
// output is a lossless, canonical view of the registry's public contract.
//
// This is the single generative source for the timeline's agent surfaces
// (JS facade, TypeScript definitions, MCP tool definitions, CLI verbs): those
// surfaces are generated from this document, never hand-maintained, so they
// cannot drift. The drift gate regenerates this artifact and asserts a
// byte-identical result against the committed copy.
//
// The result is canonical JSON (sorted keys, no insignificant whitespace), so
// the same registry always yields byte-identical output regardless of the
// order in which types or fields were registered.
runtime::Result<std::string, PersistenceError>
emit_schema_manifest(const SchemaRegistry& registry, std::size_t maximum_bytes = 8ull * 1024ull *
                                                                                 1024ull);

// The manifest-format version emitted into the document. Bump only when the
// emitted shape changes in a way downstream generators must notice; the
// committed artifact must be regenerated in the same change.
inline constexpr std::uint32_t schema_manifest_version = 1;

} // namespace pulp::timeline
