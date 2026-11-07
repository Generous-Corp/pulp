#include <pulp/host/voice_template.hpp>

#include <pulp/host/identity_text.hpp>
#include <pulp/runtime/crypto.hpp>

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <set>
#include <string_view>
#include <utility>

namespace pulp::host {
namespace {
constexpr std::uint32_t kSlots = 8;
constexpr std::uint32_t kSchemaVersion = 1;
constexpr std::string_view kWireSchema = "pulp.graph.voice-bank.state";

bool valid_enum(VoiceBindingTarget value) noexcept {
    return static_cast<std::uint8_t>(value) <=
           static_cast<std::uint8_t>(VoiceBindingTarget::Parameter);
}
bool valid_enum(VoiceBindingType value) noexcept {
    return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(VoiceBindingType::Control);
}
bool valid_enum(VoiceBindingDirection value) noexcept {
    return static_cast<std::uint8_t>(value) <=
           static_cast<std::uint8_t>(VoiceBindingDirection::Output);
}
bool valid_enum(VoiceBindingShape value) noexcept {
    return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(VoiceBindingShape::Stereo);
}
bool valid_enum(VoiceBindingRate value) noexcept {
    return static_cast<std::uint8_t>(value) <= static_cast<std::uint8_t>(VoiceBindingRate::Event);
}

bool normalize_segment(std::string_view raw, std::string& out) {
    if (raw.empty() || raw.find('/') != std::string_view::npos)
        return false;
    return normalize_authored_identity_text(raw, out);
}

bool normalize_path(const std::vector<std::string>& input, std::vector<std::string>& output) {
    if (input.empty())
        return false;
    output.clear();
    output.reserve(input.size());
    for (const auto& raw : input) {
        std::string value;
        if (!normalize_segment(raw, value))
            return false;
        output.push_back(std::move(value));
    }
    return true;
}

bool normalize_name(std::string_view raw, std::string& out) {
    return normalize_segment(raw, out);
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
void path_field(std::vector<std::uint8_t>& bytes, const std::vector<std::string>& path) {
    u32(bytes, static_cast<std::uint32_t>(path.size()));
    for (const auto& segment : path)
        string_field(bytes, segment);
}

std::string path_key(const std::vector<std::string>& path) {
    std::string result;
    for (const auto& segment : path) {
        result += std::to_string(segment.size());
        result.push_back(':');
        result += segment;
        result.push_back('|');
    }
    return result;
}

struct NormalizedBinding {
    VoiceBindingSpec spec;
    std::vector<std::string> path;
    std::string target;
};

bool normalize_binding(const VoiceBindingSpec& input, NormalizedBinding& output,
                       std::string& error) {
    if (!valid_enum(input.target_kind) || !valid_enum(input.type) || !valid_enum(input.direction) ||
        !valid_enum(input.shape) || !valid_enum(input.rate)) {
        error = "binding enum is unsupported";
        return false;
    }
    if (input.shared_across_slots) {
        error = "cross-slot mutable bindings are not admitted";
        return false;
    }
    if (!normalize_path(input.path, output.path) || !normalize_name(input.target, output.target)) {
        error = "binding path and target must be canonical NFC segments";
        return false;
    }
    if (input.target_kind == VoiceBindingTarget::Parameter) {
        if (input.direction != VoiceBindingDirection::Input ||
            input.type != VoiceBindingType::Control || input.rate == VoiceBindingRate::Event) {
            error = "parameter binding has mismatched type, direction, or rate";
            return false;
        }
    } else {
        if (input.type == VoiceBindingType::Event && input.rate != VoiceBindingRate::Event) {
            error = "event port binding must use event rate";
            return false;
        }
        if (input.type != VoiceBindingType::Event && input.rate == VoiceBindingRate::Event) {
            error = "non-event port binding cannot use event rate";
            return false;
        }
    }
    output.spec = input;
    output.spec.path = output.path;
    output.spec.target = output.target;
    return true;
}

VoiceIdentityResult make_identity(const VoiceTemplateSchema& schema, std::uint32_t slot,
                                  const NormalizedBinding& binding) {
    VoiceIdentityKey key;
    key.template_namespace = schema.template_namespace;
    key.template_name = schema.template_name;
    key.template_revision = schema.template_revision;
    key.bank_schema_version = schema.bank_schema_version;
    key.slot_ordinal = slot;
    key.path = binding.path;
    if (binding.spec.target_kind == VoiceBindingTarget::Port) {
        key.has_port = true;
        key.port.direction = binding.spec.direction == VoiceBindingDirection::Output
                                 ? VoicePortDirection::Output
                                 : VoicePortDirection::Input;
        key.port.kind =
            binding.spec.type == VoiceBindingType::Audio
                ? VoicePortKind::Audio
                : (binding.spec.type == VoiceBindingType::Event ? VoicePortKind::Event
                                                                : VoicePortKind::Control);
        key.port.ordinal = binding.spec.ordinal;
    } else {
        key.has_parameter = true;
        key.parameter = binding.target;
    }
    return canonical_voice_identity(key);
}

std::vector<std::size_t> binding_order(const std::vector<NormalizedBinding>& bindings) {
    std::vector<std::size_t> order(bindings.size());
    for (std::size_t i = 0; i < order.size(); ++i)
        order[i] = i;
    std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        const auto& a = bindings[lhs];
        const auto& b = bindings[rhs];
        if (a.path != b.path)
            return a.path < b.path;
        if (a.target != b.target)
            return a.target < b.target;
        if (a.spec.target_kind != b.spec.target_kind)
            return static_cast<std::uint8_t>(a.spec.target_kind) <
                   static_cast<std::uint8_t>(b.spec.target_kind);
        if (a.spec.ordinal != b.spec.ordinal)
            return a.spec.ordinal < b.spec.ordinal;
        return static_cast<std::uint8_t>(a.spec.type) < static_cast<std::uint8_t>(b.spec.type);
    });
    return order;
}

std::string manifest_digest(const VoiceBankManifest& manifest) {
    std::vector<std::uint8_t> bytes;
    bytes.push_back(0x01);
    string_field(bytes, "pulp.graph.voice-bank.manifest");
    string_field(bytes, manifest.schema.template_namespace);
    string_field(bytes, manifest.schema.template_name);
    u32(bytes, manifest.schema.template_revision);
    u32(bytes, manifest.schema.bank_schema_version);
    std::vector<std::size_t> order;
    order.reserve(manifest.bindings.size());
    for (std::size_t i = 0; i < manifest.bindings.size(); ++i)
        order.push_back(i);
    std::sort(order.begin(), order.end(), [&](std::size_t lhs, std::size_t rhs) {
        const auto& a = manifest.bindings[lhs];
        const auto& b = manifest.bindings[rhs];
        if (a.slot_ordinal != b.slot_ordinal)
            return a.slot_ordinal < b.slot_ordinal;
        if (a.spec.path != b.spec.path)
            return a.spec.path < b.spec.path;
        return a.spec.target < b.spec.target;
    });
    u32(bytes, static_cast<std::uint32_t>(order.size()));
    for (const auto index : order) {
        const auto& item = manifest.bindings[index];
        u32(bytes, item.slot_ordinal);
        path_field(bytes, item.spec.path);
        string_field(bytes, item.spec.target);
        bytes.push_back(static_cast<std::uint8_t>(item.spec.target_kind));
        bytes.push_back(static_cast<std::uint8_t>(item.spec.type));
        bytes.push_back(static_cast<std::uint8_t>(item.spec.direction));
        bytes.push_back(static_cast<std::uint8_t>(item.spec.shape));
        bytes.push_back(static_cast<std::uint8_t>(item.spec.rate));
        u32(bytes, item.spec.ordinal);
    }
    return runtime::sha256_hex(bytes.data(), bytes.size());
}

bool manifest_matches(const VoiceBankManifest& manifest) {
    return manifest.bindings.size() == kSlots * manifest.schema.bindings.size() &&
           !manifest.sha256.empty() && manifest.sha256 == manifest_digest(manifest);
}

bool valid_parameter(const VoiceBankManifest& manifest, const VoiceParameterValue& value) {
    if (value.slot_ordinal >= kSlots || !std::isfinite(value.value))
        return false;
    std::vector<std::string> path;
    if (!normalize_path(value.path, path))
        return false;
    for (const auto& binding : manifest.bindings) {
        if (binding.slot_ordinal == value.slot_ordinal &&
            binding.spec.target_kind == VoiceBindingTarget::Parameter &&
            binding.spec.path == path && binding.spec.target == value.target)
            return true;
    }
    return false;
}

} // namespace

VoiceBankPrepareResult prepare_voice_bank(const VoiceTemplateSchema& schema) {
    VoiceBankPrepareResult result;
    if (schema.template_revision == 0 || schema.bank_schema_version == 0 ||
        schema.bank_schema_version != kSchemaVersion) {
        result.error = "unsupported template or bank schema revision";
        return result;
    }
    std::string name;
    if (!normalize_name(schema.template_namespace, name) ||
        !normalize_name(schema.template_name, name)) {
        result.error = "template identity must be canonical NFC segments";
        return result;
    }
    if (schema.bindings.empty()) {
        result.error = "template must declare at least one binding";
        return result;
    }

    std::vector<NormalizedBinding> normalized;
    normalized.reserve(schema.bindings.size());
    std::set<std::string> declaration_keys;
    for (const auto& raw : schema.bindings) {
        NormalizedBinding binding;
        if (!normalize_binding(raw, binding, result.error))
            return result;
        std::string key = path_key(binding.path) + "\x1f" + binding.target + "\x1f" +
                          std::to_string(static_cast<unsigned>(binding.spec.target_kind)) + "\x1f" +
                          std::to_string(binding.spec.ordinal);
        if (!declaration_keys.insert(std::move(key)).second) {
            result.error = "duplicate or aliased binding declaration";
            return result;
        }
        normalized.push_back(std::move(binding));
    }
    const auto order = binding_order(normalized);
    result.manifest.schema = schema;
    normalize_name(schema.template_namespace, result.manifest.schema.template_namespace);
    normalize_name(schema.template_name, result.manifest.schema.template_name);
    result.manifest.schema.bindings.clear();
    for (const auto index : order)
        result.manifest.schema.bindings.push_back(normalized[index].spec);
    result.manifest.bindings.reserve(kSlots * normalized.size());
    for (std::uint32_t slot = 0; slot < kSlots; ++slot) {
        for (const auto index : order) {
            VoiceSlotBinding item;
            item.slot_ordinal = slot;
            item.spec = normalized[index].spec;
            item.identity = make_identity(schema, slot, normalized[index]);
            if (!item.identity.valid) {
                result.failing_slot = slot;
                result.failing_path = path_key(normalized[index].path);
                result.error =
                    item.identity.error.empty() ? "binding identity refused" : item.identity.error;
                result.manifest = {};
                return result;
            }
            result.manifest.bindings.push_back(std::move(item));
        }
    }
    result.manifest.sha256 = manifest_digest(result.manifest);
    result.valid = true;
    return result;
}

VoiceBankPersistenceResult encode_voice_bank_state(const VoiceBankManifest& manifest,
                                                   const VoiceBankState& state) {
    VoiceBankPersistenceResult result;
    if (!manifest_matches(manifest)) {
        result.error = "manifest is not a verified prepared bank";
        return result;
    }
    if (state.schema_version != kSchemaVersion || state.manifest_sha256 != manifest.sha256) {
        result.error = "state schema or manifest identity does not match";
        return result;
    }
    std::vector<VoiceParameterValue> values = state.parameters;
    for (const auto& value : values) {
        if (!valid_parameter(manifest, value)) {
            result.error = "state contains an undeclared, duplicate, or non-finite parameter";
            return result;
        }
    }
    std::sort(values.begin(), values.end(),
              [](const VoiceParameterValue& a, const VoiceParameterValue& b) {
                  if (a.slot_ordinal != b.slot_ordinal)
                      return a.slot_ordinal < b.slot_ordinal;
                  if (a.path != b.path)
                      return a.path < b.path;
                  return a.target < b.target;
              });
    for (std::size_t i = 1; i < values.size(); ++i) {
        if (values[i - 1].slot_ordinal == values[i].slot_ordinal &&
            values[i - 1].path == values[i].path && values[i - 1].target == values[i].target) {
            result.error = "state contains a duplicate parameter";
            return result;
        }
    }
    result.encoding.push_back(0x01);
    string_field(result.encoding, kWireSchema);
    u32(result.encoding, state.schema_version);
    string_field(result.encoding, manifest.sha256);
    u32(result.encoding, static_cast<std::uint32_t>(values.size()));
    for (const auto& value : values) {
        u32(result.encoding, value.slot_ordinal);
        path_field(result.encoding, value.path);
        string_field(result.encoding, value.target);
        std::uint32_t bits = 0;
        static_assert(sizeof(bits) == sizeof(value.value));
        std::memcpy(&bits, &value.value, sizeof(bits));
        u32(result.encoding, bits);
    }
    result.sha256 = runtime::sha256_hex(result.encoding.data(), result.encoding.size());
    result.valid = true;
    return result;
}

bool restore_voice_bank_state(const VoiceBankManifest& manifest, const VoiceBankState& candidate,
                              VoiceBankState& destination, std::string* error) {
    auto fail = [&](const char* message) {
        if (error)
            *error = message;
        return false;
    };
    if (!manifest_matches(manifest))
        return fail("manifest is not a verified prepared bank");
    if (candidate.schema_version != kSchemaVersion || candidate.manifest_sha256 != manifest.sha256)
        return fail("state schema or manifest identity does not match");
    std::set<std::string> seen;
    for (const auto& value : candidate.parameters) {
        if (!valid_parameter(manifest, value))
            return fail("state contains an undeclared or non-finite parameter");
        const auto key = std::to_string(value.slot_ordinal) + "\x1f" + path_key(value.path) +
                         "\x1f" + value.target;
        if (!seen.insert(key).second)
            return fail("state contains a duplicate parameter");
    }
    destination = candidate;
    return true;
}

} // namespace pulp::host
