#pragma once

// Value-only authored identity for graph voice-bank slots.  Runtime node
// handles, allocator indexes, epochs, and note generations are deliberately
// absent from this type and can never affect its encoding.

#include <cstdint>
#include <string>
#include <vector>

namespace pulp::host {

enum class VoicePortDirection : std::uint8_t { Input = 0, Output = 1 };
enum class VoicePortKind : std::uint8_t { Audio = 0, Event = 1, Control = 2 };

struct VoicePortKey {
    VoicePortDirection direction = VoicePortDirection::Input;
    VoicePortKind kind = VoicePortKind::Audio;
    std::uint32_t ordinal = 0;
};

/// Canonical authored identity for one template/bank/slot/path target.
/// `path` contains explicit stable segments; it is never a display label or
/// runtime node path.  A key may identify a path alone, a port, a parameter,
/// or both targets at that path.
struct VoiceIdentityKey {
    std::string template_namespace;
    std::string template_name;
    std::uint32_t template_revision = 0;
    std::uint32_t bank_schema_version = 1;
    std::uint32_t slot_ordinal = 0;
    std::vector<std::string> path;
    bool has_port = false;
    VoicePortKey port{};
    bool has_parameter = false;
    std::string parameter;
};

struct VoiceIdentityResult {
    bool valid = false;
    std::vector<std::uint8_t> encoding;
    std::string sha256;
    std::string error;
};

/// Encode and hash an authored identity using the G-4 v1 tagged,
/// length-delimited wire form. Strings are NFC UTF-8 and length-prefixed by
/// big-endian uint32; integer fields are big-endian. Returns an invalid result
/// without bytes or a digest when the key is not canonical.
VoiceIdentityResult canonical_voice_identity(const VoiceIdentityKey& key);

} // namespace pulp::host
