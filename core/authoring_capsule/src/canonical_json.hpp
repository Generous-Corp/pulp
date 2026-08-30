#pragma once

/// @file canonical_json.hpp
/// Canonical JSON — the exact byte form every capsule digest is taken over.
///
/// This unit owns one invariant: the same logical envelope produces the same
/// bytes on every machine, every toolchain, and every run. `revision_id` is a
/// SHA-256 over those bytes, so any wobble here — a key order that follows
/// insertion, a number printed by the platform's `printf`, a stray space —
/// makes two exports of an unchanged project disagree about their identity.
/// The canonical form is specified byte-for-byte at the top of
/// `canonical_json.cpp`; read it before changing anything in this unit.
///
/// The parser is choc's; the serializer is Pulp's. choc's writer is not usable
/// here because it emits members in insertion order and formats numbers with
/// `std::ostringstream`, neither of which is canonical.
///
/// Everything here is envelope-shaped work over untrusted bytes: it never
/// executes anything, never touches the filesystem, and fails closed with a
/// `CapsuleError` naming the offending field rather than throwing.

#include <pulp/authoring_capsule/status.hpp>
#include <pulp/runtime/result.hpp>

#include <choc/containers/choc_Value.h>

#include <cstdint>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace pulp::authoring_capsule::detail {

/// The value model. Parsing, building, and serializing all speak choc values
/// so an unknown key is carried as data rather than reconstructed from a
/// guess about its shape.
using Json = choc::value::Value;
using JsonView = choc::value::ValueView;

/// Nesting bound applied before choc's recursive-descent parser sees the text,
/// and again while serializing. Both recurse, so a hostile envelope of ten
/// thousand open braces would otherwise exhaust the stack — a crash, not a
/// rejection. Far above anything a real envelope nests.
inline constexpr std::size_t kMaxJsonDepth = 128;

// ── Parsing ──────────────────────────────────────────────────────────────
//
// `subject` is the RFC 6901 pointer (or other locator) reported back in
// `CapsuleError::subject` so a failure names the field a person has to fix.

/// Parse a JSON object or array. Rejects trailing content after the top-level
/// value, embedded NUL bytes, and nesting past `kMaxJsonDepth`.
///
/// Parsing copies `text`, so the caller applies the size budget first —
/// `CapsuleLimits::max_manifest_bytes` for the envelope. This does not enforce
/// one, because it is also used on subtrees already inside that budget.
runtime::Result<Json, CapsuleError> parse_json(std::string_view text, std::string_view subject = {});

/// `parse_json`, additionally requiring the top-level value to be an object.
runtime::Result<Json, CapsuleError> parse_json_object(std::string_view text,
                                                      std::string_view subject = {});

// ── Serializing ──────────────────────────────────────────────────────────

/// Serialize to the canonical form. Fails on a non-finite number, on a string
/// that is not well-formed UTF-8, and on nesting past `kMaxJsonDepth`;
/// `CapsuleError::subject` carries the RFC 6901 pointer of the offending node,
/// rooted at `pointer_root`.
runtime::Result<std::string, CapsuleError> to_canonical_text(const JsonView& value,
                                                             std::string_view pointer_root = {});

/// Parse then re-serialize: the normalizing round-trip used on the envelope's
/// profile-owned JSON blobs (`topology`, `compatibility.extra`, `provenance`,
/// `distribution`, preserved unknown keys) so they contribute stable bytes to
/// the digest no matter how the writer that produced them spaced its output.
runtime::Result<std::string, CapsuleError> canonicalize_json_text(std::string_view text,
                                                                  std::string_view subject = {});

// ── Numbers ──────────────────────────────────────────────────────────────

/// The canonical decimal for a double: shortest round-tripping digits, no
/// exponent or decimal point when the value is an exact integer in range.
/// Fails on NaN and infinity, which have no JSON spelling. Exposed so a test
/// can pin the rule directly rather than inferring it from a whole envelope.
runtime::Result<std::string, CapsuleError> canonical_number(double value,
                                                            std::string_view subject = {});

/// The canonical decimal for an integer. Always exact, never fails.
std::string canonical_number(std::int64_t value);

// ── Building ─────────────────────────────────────────────────────────────
//
// Insertion order is irrelevant — the serializer sorts. Build in whatever
// order reads best at the call site.

Json json_object();
Json json_array();
Json json_string_array(const std::vector<std::string>& values);

/// Set a member, replacing any existing member of the same name. Replacing
/// rather than appending keeps a builder mistake from producing a duplicate
/// key, which has no canonical serialization.
void put(Json& object, std::string_view key, const JsonView& value);
void put_string(Json& object, std::string_view key, std::string_view value);
void put_int(Json& object, std::string_view key, std::int64_t value);
void put_uint(Json& object, std::string_view key, std::uint64_t value);
void put_bool(Json& object, std::string_view key, bool value);

/// Attach JSON *text* (an object or array held as a string on `Manifest`) as a
/// real subtree, so it is canonicalized with everything else instead of being
/// spliced in as an opaque string.
runtime::Result<void, CapsuleError> put_json_text(Json& object, std::string_view key,
                                                  std::string_view json_text,
                                                  std::string_view subject = {});

void append(Json& array, const JsonView& value);

// ── Unknown-key preservation ─────────────────────────────────────────────

/// The members of `source` whose names are not in `known`, as an object.
///
/// An unknown *optional* key must survive a round-trip through a reader that
/// predates it: dropping it would silently discard metadata a newer writer
/// emitted and, because the digest covers the envelope, would also change the
/// revision identity of a project nobody edited.
Json unknown_members(const JsonView& source, std::span<const std::string_view> known);

/// Copy each member of `extra` into `object` that `object` does not already
/// have. A key the envelope owns is never overwritten, so preserved metadata
/// can never forge a routing input such as `profile` or `required_capabilities`.
///
/// `extra` must not be a view into `object`: growing `object` moves its packed
/// storage, which would leave such a view dangling mid-merge.
void merge_absent_members(Json& object, const JsonView& extra);

// ── Reading ──────────────────────────────────────────────────────────────

bool has_member(const JsonView& object, std::string_view key);

/// A required string member. Fails `manifest_invalid` when absent or when the
/// member is present with another type.
runtime::Result<std::string, CapsuleError> require_string(const JsonView& object,
                                                          std::string_view key,
                                                          std::string_view pointer_root = {});

/// A required non-negative integer member.
runtime::Result<std::uint64_t, CapsuleError> require_uint(const JsonView& object,
                                                          std::string_view key,
                                                          std::string_view pointer_root = {});

/// An optional member. A present member of the wrong type is an error rather
/// than a silent fallback: a `bytes` field spelled as a string is a defect in
/// the writer, and answering 0 would let a closure check pass on a lie.
runtime::Result<std::string, CapsuleError> optional_string(const JsonView& object,
                                                           std::string_view key,
                                                           std::string_view fallback = {},
                                                           std::string_view pointer_root = {});
runtime::Result<std::uint64_t, CapsuleError> optional_uint(const JsonView& object,
                                                           std::string_view key,
                                                           std::uint64_t fallback = 0,
                                                           std::string_view pointer_root = {});
runtime::Result<bool, CapsuleError> optional_bool(const JsonView& object, std::string_view key,
                                                  bool fallback = false,
                                                  std::string_view pointer_root = {});
runtime::Result<std::vector<std::string>, CapsuleError>
optional_string_array(const JsonView& object, std::string_view key,
                      std::string_view pointer_root = {});

/// The canonical text of a nested object or array member, or `fallback` when
/// the member is absent. This is how a profile-owned subtree reaches the
/// `Manifest`'s `*_json` string fields without losing its canonical form.
runtime::Result<std::string, CapsuleError> optional_json_text(const JsonView& object,
                                                              std::string_view key,
                                                              std::string_view fallback,
                                                              std::string_view pointer_root = {});

// ── Subtree and digest ───────────────────────────────────────────────────

/// A new object holding only the named members of `object` that exist, in
/// their original values. This is the operator that builds the semantic
/// subtree `revision_id` is taken over: the digest is defined by what it
/// *includes*, so a descriptive key added later cannot disturb it.
Json subtree(const JsonView& object, std::span<const std::string_view> keys);

/// Reorder an array of objects by a string member, in byte order — the rule
/// the format states for `files[]`. Fails when an element is not an object or
/// lacks the key, since a partial order is not a canonical one.
runtime::Result<void, CapsuleError> sort_array_by_string_member(Json& array, std::string_view key,
                                                                std::string_view pointer_root = {});

/// Lowercase hex SHA-256 over the canonical bytes of `value`.
runtime::Result<std::string, CapsuleError> canonical_sha256_hex(const JsonView& value);

/// `canonical_sha256_hex` in the `sha256:<hex>` form the envelope writes for
/// `revision_id` and `parent_revision`. The algorithm travels with the digest
/// so a future second hash is a new prefix, not an ambiguous 64 hex characters.
runtime::Result<std::string, CapsuleError> canonical_sha256_uri(const JsonView& value);

}  // namespace pulp::authoring_capsule::detail
