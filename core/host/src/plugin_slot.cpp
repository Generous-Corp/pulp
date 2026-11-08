// PluginSlot::load() — format dispatcher.
//
// Routes the load request to the format-specific loader. Each loader lives
// in its own translation unit (plugin_slot_clap.cpp, plugin_slot_vst3.cpp,
// …) and is compiled in only when the relevant SDK is available, guarded
// by PULP_HOST_HAS_<FORMAT> compile definitions in core/host/CMakeLists.txt.

#include <pulp/host/plugin_slot.hpp>
#include <pulp/host/timeline_device_resolver.hpp>
#include <pulp/runtime/log.hpp>
#include <choc/text/choc_JSON.h>

#include <mutex>

namespace pulp::host {

namespace {
std::mutex& host_identity_mutex() {
    static std::mutex mutex;
    return mutex;
}
PluginHostIdentity& host_identity_storage() {
    static PluginHostIdentity identity;
    return identity;
}
} // namespace

void set_plugin_host_identity(PluginHostIdentity identity) {
    std::lock_guard<std::mutex> lock(host_identity_mutex());
    host_identity_storage() = std::move(identity);
}

PluginHostIdentity plugin_host_identity() {
    std::lock_guard<std::mutex> lock(host_identity_mutex());
    return host_identity_storage();
}

#if PULP_HOST_HAS_CLAP
std::unique_ptr<PluginSlot> load_clap_plugin(const PluginInfo& info);
#endif
#if PULP_HOST_HAS_AU
std::unique_ptr<PluginSlot> load_au_plugin(const PluginInfo& info);
#endif
#if PULP_HOST_HAS_VST3
std::unique_ptr<PluginSlot> load_vst3_plugin(const PluginInfo& info);
#endif
#if PULP_HOST_HAS_LV2
std::unique_ptr<PluginSlot> load_lv2_plugin(const PluginInfo& info);
#endif

std::unique_ptr<PluginSlot> PluginSlot::load(const PluginInfo& requested) {
    auto info = requested;
    // A prior capture cannot certify a new best-effort load or its fallback.
    info.identity_evidence.reset();
    switch (info.format) {
        case PluginFormat::CLAP:
#if PULP_HOST_HAS_CLAP
            return load_clap_plugin(info);
#else
            runtime::log_warn("PluginSlot::load: CLAP loader not compiled in");
            return nullptr;
#endif
        case PluginFormat::AudioUnit:
        case PluginFormat::AudioUnitV3:
#if PULP_HOST_HAS_AU
            return load_au_plugin(info);
#else
            runtime::log_warn("PluginSlot::load: AU loader only available on macOS");
            return nullptr;
#endif
        case PluginFormat::VST3:
#if PULP_HOST_HAS_VST3
            return load_vst3_plugin(info);
#else
            runtime::log_warn("PluginSlot::load: VST3 loader not compiled in");
            return nullptr;
#endif
        case PluginFormat::LV2:
#if PULP_HOST_HAS_LV2
            return load_lv2_plugin(info);
#else
            runtime::log_warn("PluginSlot::load: LV2 loader not compiled in");
            return nullptr;
#endif
    case PluginFormat::BuiltIn:
        return load_builtin_plugin(info);
    }
    return nullptr;
}

std::optional<PluginIdentityError> validate_plugin_type_identity(const PluginTypeIdentity& type) {
    if (type.format != "clap")
        return PluginIdentityError::unsupported_format;
    const auto& id = type.descriptor_id;
    if (id.empty())
        return PluginIdentityError::native_identity_missing;
    if (id.size() > 4096)
        return PluginIdentityError::invalid_native_identity;
    for (std::size_t i = 0; i < id.size();) {
        auto first = static_cast<unsigned char>(id[i++]);
        std::uint32_t cp = first;
        unsigned trailing = 0;
        if (first >= 0xc2 && first <= 0xdf) {
            cp = first & 0x1f;
            trailing = 1;
        } else if (first >= 0xe0 && first <= 0xef) {
            cp = first & 0x0f;
            trailing = 2;
        } else if (first >= 0xf0 && first <= 0xf4) {
            cp = first & 7;
            trailing = 3;
        } else if (first >= 0x80)
            return PluginIdentityError::invalid_native_identity;
        if (i + trailing > id.size())
            return PluginIdentityError::invalid_native_identity;
        for (unsigned j = 0; j < trailing; ++j) {
            auto c = static_cast<unsigned char>(id[i++]);
            if ((c & 0xc0) != 0x80)
                return PluginIdentityError::invalid_native_identity;
            cp = (cp << 6) | (c & 0x3f);
        }
        if ((trailing == 1 && cp < 0x80) || (trailing == 2 && cp < 0x800) ||
            (trailing == 3 && cp < 0x10000) || cp > 0x10ffff || (cp >= 0xd800 && cp <= 0xdfff) ||
            cp < 0x20 || (cp >= 0x7f && cp <= 0x9f))
            return PluginIdentityError::invalid_native_identity;
    }
    return std::nullopt;
}

std::variant<CanonicalPluginIdentity, PluginIdentityError>
canonical_plugin_identity(const PluginIdentityEvidence& evidence) {
    if (auto error = validate_plugin_type_identity(evidence.type()))
        return *error;
    // A fixed field order and JSON's reversible quoting preserve the exact ID bytes.
    const auto quoted = choc::json::toString(choc::value::Value(evidence.type().descriptor_id));
    return CanonicalPluginIdentity{
        "{\"format\":\"clap\",\"native_type\":{\"descriptor_id\":" + quoted + "}}", evidence};
}

std::string serialize_plugin_identity_evidence(const PluginIdentityEvidence& evidence) {
    auto object = choc::value::createObject("");
    object.addMember("schema", "pulp.clap.identity-evidence.v1");
    object.addMember("format", evidence.type().format);
    object.addMember("descriptor_id", evidence.type().descriptor_id);
    object.addMember("origin", evidence.origin() == PluginIdentityOrigin::clap_descriptor
                                   ? "clap_descriptor"
                                   : "authored_assertion");
    object.addMember("source_contract", evidence.source_contract_revision());
    object.addMember("verification", evidence.verification() == PluginIdentityVerification::loaded
                                         ? "loaded"
                                         : "declared");
    object.addMember("sha256", evidence.artifact().sha256);
    object.addMember("byte_length", static_cast<std::int64_t>(evidence.artifact().byte_length));
    object.addMember("descriptor_version",
                     evidence.artifact().descriptor_version
                         ? choc::value::Value(*evidence.artifact().descriptor_version)
                         : choc::value::Value());
    return choc::json::toString(object);
}

std::optional<PluginIdentityEvidence> parse_plugin_identity_evidence(std::string_view text) {
    try {
        auto object = choc::json::parse(std::string(text));
        if (!object.isObject() || object["schema"].toString() != "pulp.clap.identity-evidence.v1")
            return std::nullopt;
        PluginTypeIdentity type{object["format"].toString(), object["descriptor_id"].toString()};
        if (validate_plugin_type_identity(type))
            return std::nullopt;
        const auto origin = object["origin"].toString();
        if (origin != "clap_descriptor" && origin != "authored_assertion")
            return std::nullopt;
        const auto length = object["byte_length"].getInt64();
        if (length < 0)
            return std::nullopt;
        PluginArtifactProvenance artifact{object["sha256"].toString(),
                                          static_cast<std::uint64_t>(length), std::nullopt};
        if (!object["descriptor_version"].isVoid()) {
            if (!object["descriptor_version"].isString())
                return std::nullopt;
            artifact.descriptor_version = object["descriptor_version"].toString();
        }
        return PluginIdentityEvidence::declared(
            std::move(type),
            origin == "clap_descriptor" ? PluginIdentityOrigin::clap_descriptor
                                        : PluginIdentityOrigin::authored_assertion,
            object["source_contract"].toString(), std::move(artifact));
    } catch (...) {
        return std::nullopt;
    }
}

PluginLoadResult PluginSlot::load_exact(const PluginSelection& selection) {
    if (auto error = validate_plugin_type_identity(selection.type))
        return {*error};
#if PULP_HOST_HAS_CLAP
    PluginLoadResult load_clap_plugin_exact(const PluginSelection&);
    return load_clap_plugin_exact(selection);
#else
    return {PluginIdentityError::unsupported_format};
#endif
}

} // namespace pulp::host
