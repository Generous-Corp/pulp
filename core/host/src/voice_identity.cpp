#include <pulp/host/voice_identity.hpp>

#include <pulp/host/identity_text.hpp>
#include <pulp/runtime/crypto.hpp>

#include <limits>
#include <string_view>

namespace pulp::host {
namespace {

constexpr std::string_view kSchema = "pulp.graph.voice-bank";
constexpr std::uint32_t kSchemaVersion = 1;
constexpr std::uint32_t kSlotCount = 8;

bool normalize(std::string_view value, std::string& out) {
    if (value.size() > std::numeric_limits<std::uint32_t>::max())
        return false;
    return normalize_authored_identity_text(value, out);
}

bool segment(std::string_view value, std::string& out) {
    if (value.find('/') != std::string_view::npos)
        return false;
    return normalize(value, out);
}

void u32(std::vector<std::uint8_t>& bytes, std::uint32_t value) {
    bytes.push_back(static_cast<std::uint8_t>(value >> 24));
    bytes.push_back(static_cast<std::uint8_t>(value >> 16));
    bytes.push_back(static_cast<std::uint8_t>(value >> 8));
    bytes.push_back(static_cast<std::uint8_t>(value));
}

void string_field(std::vector<std::uint8_t>& bytes, std::string_view value) {
    u32(bytes, static_cast<std::uint32_t>(value.size()));
    bytes.insert(bytes.end(), value.begin(), value.end());
}

VoiceIdentityResult invalid(const char* error) {
    VoiceIdentityResult result;
    result.error = error;
    return result;
}

} // namespace

VoiceIdentityResult canonical_voice_identity(const VoiceIdentityKey& key) {
    if (key.template_revision == 0 || key.bank_schema_version == 0)
        return invalid("template and bank schema revisions must be nonzero");
    if (key.slot_ordinal >= kSlotCount)
        return invalid("slot ordinal is outside the eight-slot bank");

    std::string ns, name;
    if (!segment(key.template_namespace, ns) || !segment(key.template_name, name))
        return invalid("template identity must be canonical NFC UTF-8 without slash ambiguity");
    if (key.path.empty())
        return invalid("voice identity path must contain at least one segment");
    std::vector<std::string> path;
    path.reserve(key.path.size());
    for (const auto& raw : key.path) {
        std::string normalized;
        if (!segment(raw, normalized))
            return invalid("voice identity path segments must be canonical and slash-free");
        path.push_back(std::move(normalized));
    }
    std::string parameter;
    if (key.has_parameter && !segment(key.parameter, parameter))
        return invalid("voice parameter identity must be canonical and slash-free");
    if (key.has_port && (static_cast<std::uint8_t>(key.port.direction) >
                             static_cast<std::uint8_t>(VoicePortDirection::Output) ||
                         static_cast<std::uint8_t>(key.port.kind) >
                             static_cast<std::uint8_t>(VoicePortKind::Control)))
        return invalid("voice port identity has an unsupported direction or kind");
    if (path.size() > std::numeric_limits<std::uint32_t>::max())
        return invalid("voice identity path has too many segments");

    VoiceIdentityResult result;
    result.valid = true;
    // Tag + schema + versions + template + slot + path + target presence.
    result.encoding.reserve(64);
    result.encoding.push_back(0x01);
    string_field(result.encoding, kSchema);
    u32(result.encoding, kSchemaVersion);
    string_field(result.encoding, ns);
    string_field(result.encoding, name);
    u32(result.encoding, key.template_revision);
    u32(result.encoding, key.bank_schema_version);
    u32(result.encoding, key.slot_ordinal);
    u32(result.encoding, static_cast<std::uint32_t>(path.size()));
    for (const auto& item : path)
        string_field(result.encoding, item);
    result.encoding.push_back(key.has_port ? 1 : 0);
    if (key.has_port) {
        result.encoding.push_back(static_cast<std::uint8_t>(key.port.direction));
        result.encoding.push_back(static_cast<std::uint8_t>(key.port.kind));
        u32(result.encoding, key.port.ordinal);
    }
    result.encoding.push_back(key.has_parameter ? 1 : 0);
    if (key.has_parameter)
        string_field(result.encoding, parameter);
    result.sha256 = runtime::sha256_hex(result.encoding.data(), result.encoding.size());
    return result;
}

} // namespace pulp::host
