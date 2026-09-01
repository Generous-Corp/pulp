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
#include <string>
#include <string_view>

namespace pulp::authoring_capsule::detail {

/// The value model. Parsing and serializing both speak choc values so an
/// unknown key is carried as data rather than reconstructed from a guess
/// about its shape.
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

// There is deliberately no digest helper here. `revision_digest()` in
// manifest.cpp is the one place that turns canonical bytes into an identity,
// and a second implementation of the same spelling in this file would be free
// to drift from it — silently, since both would keep producing plausible
// 64-character strings. Canonicalize with `to_canonical_text` and hash at the
// single call site that owns the meaning.

}  // namespace pulp::authoring_capsule::detail
