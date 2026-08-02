#pragma once

#include "serialize_internal.hpp"

#include <pulp/timeline/serialize.hpp>

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace pulp::timeline::detail {

template <typename T>
runtime::Result<T, PersistenceError>
decode_fail(PersistenceErrorCode code, std::string path = {}, std::size_t byte_offset = 0,
            std::uint64_t actual = 0, std::uint64_t limit = 0) {
    return runtime::Result<T, PersistenceError>(runtime::Err(
        PersistenceError{code, byte_offset, actual, limit, std::move(path), std::nullopt}));
}

runtime::Result<const JsonValue*, PersistenceError>
required_decode_member(const JsonValue& object_value, std::string_view name, std::string path);

runtime::Result<std::string, PersistenceError>
decode_string_field(const JsonValue& object_value, std::string_view name, std::string path);

runtime::Result<timebase::RationalRate, PersistenceError>
decode_rational_rate(const JsonValue& value, std::string path);

// Decodes the sparse modifier companion array of a note set. A modifier names a
// note, so the array can never outgrow the notes it annotates: `note_budget`
// bounds it against that same count and keeps a hostile payload from spending
// unbounded memory on annotations alone. Structural validity beyond the field
// shapes — every named note existing, no neutral entry — belongs to the model.
runtime::Result<std::vector<NoteModifier>, PersistenceError>
decode_note_modifiers(const JsonValue& value, std::size_t note_budget, std::string path);

runtime::Result<std::optional<TrackFreeze>, PersistenceError>
decode_track_freeze(const JsonValue* freeze, std::string path);

// An absent member is how the canonical form spells a default mixer, so this
// yields the default rather than refusing.
runtime::Result<TrackMixer, PersistenceError> decode_track_mixer(const JsonValue* mixer,
                                                                 std::string path);

} // namespace pulp::timeline::detail
