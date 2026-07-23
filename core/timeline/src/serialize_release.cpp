#include <pulp/timeline/serialize.hpp>

#include <algorithm>
#include <limits>
#include <string>

namespace pulp::timeline {
namespace {

struct RewriteSuccess {};

template <typename T>
runtime::Result<T, PersistenceError> fail(PersistenceErrorCode code, std::string path = {},
                                          std::uint64_t actual = 0, std::uint64_t limit = 0) {
    return runtime::Result<T, PersistenceError>(
        runtime::Err(PersistenceError{code, 0, actual, limit, std::move(path)}));
}

JsonValue* member(JsonValue& object, std::string_view name) noexcept {
    if (object.kind != JsonValue::Kind::Object)
        return nullptr;
    const auto found = std::find_if(object.object.begin(), object.object.end(),
                                    [name](const auto& item) { return item.first == name; });
    return found == object.object.end() ? nullptr : &found->second;
}

std::string child_path(std::string_view parent, std::string_view child) {
    std::string result(parent);
    if (result.empty())
        result = "/";
    else if (result.back() != '/')
        result.push_back('/');
    result.append(child);
    return result;
}

bool has_downgrade_path(const TypeSchema& schema, std::uint32_t target_version) noexcept {
    auto version = schema.current_version;
    while (version != target_version) {
        const auto found =
            std::find_if(schema.downgrades.begin(), schema.downgrades.end(),
                         [version](const auto& step) { return step.from_version == version; });
        if (found == schema.downgrades.end() || !found->migrate || found->to_version >= version ||
            found->to_version < target_version)
            return false;
        version = found->to_version;
    }
    return true;
}

runtime::Result<RewriteSuccess, PersistenceError>
validate_release_map(const SchemaReleaseMap& release, const SchemaRegistry& registry) {
    if (release.release_label.empty() || !is_valid_utf8(release.release_label) ||
        release.versions.empty())
        return fail<RewriteSuccess>(PersistenceErrorCode::InvalidSchema, "/release_label");
    for (std::size_t index = 0; index < release.versions.size(); ++index) {
        const auto& target = release.versions[index];
        const auto path = "/versions/" + std::to_string(index);
        if (!SchemaIdentity{std::string(target.type_name), target.version}.valid())
            return fail<RewriteSuccess>(PersistenceErrorCode::InvalidSchema, path);
        const auto duplicate = std::find_if(
            release.versions.begin(), release.versions.begin() + index, [&](const auto& previous) {
                return previous.domain == target.domain && previous.type_name == target.type_name;
            });
        if (duplicate != release.versions.begin() + index)
            return fail<RewriteSuccess>(PersistenceErrorCode::InvalidSchema, path);
        const auto* schema = registry.find(target.domain, target.type_name);
        if (!schema)
            return fail<RewriteSuccess>(PersistenceErrorCode::UnsupportedStructuralType, path);
        if (target.version > schema->current_version)
            return fail<RewriteSuccess>(PersistenceErrorCode::UnsupportedSchemaVersion, path,
                                        target.version, schema->current_version);
        if (!has_downgrade_path(*schema, target.version))
            return fail<RewriteSuccess>(PersistenceErrorCode::MigrationPathMissing, path);
    }
    if (!release.find(SchemaDomain::Document, "pulp.timeline.project"))
        return fail<RewriteSuccess>(PersistenceErrorCode::UnsupportedStructuralType, "/versions");
    return runtime::Ok(RewriteSuccess{});
}

class ReleaseRewriter {
  public:
    ReleaseRewriter(const SchemaRegistry& registry, const SchemaReleaseMap& release,
                    std::size_t maximum, std::size_t current_size)
        : registry_(registry), release_(release), maximum_(maximum), current_size_(current_size) {}

    runtime::Result<RewriteSuccess, PersistenceError> rewrite(JsonValue& envelope,
                                                              SchemaDomain domain,
                                                              std::string_view expected_type,
                                                              const std::string& path) const {
        auto* data = member(envelope, "data");
        auto* type_value = member(envelope, "type_name");
        auto* version_value = member(envelope, "version");
        if (envelope.object.size() != 3 || !data || data->kind != JsonValue::Kind::Object ||
            !type_value || type_value->kind != JsonValue::Kind::String || !version_value)
            return fail<RewriteSuccess>(PersistenceErrorCode::InvalidSchema, path);
        if (!expected_type.empty() && type_value->scalar != expected_type)
            return fail<RewriteSuccess>(PersistenceErrorCode::UnsupportedStructuralType,
                                        child_path(path, "type_name"));

        const std::string type_name = type_value->scalar;
        auto source_version = parse_u32_number(*version_value, child_path(path, "version"));
        if (!source_version)
            return runtime::Result<RewriteSuccess, PersistenceError>(
                runtime::Err(source_version.error()));
        const auto* target = release_.find(domain, type_name);
        if (!target)
            return fail<RewriteSuccess>(PersistenceErrorCode::UnsupportedStructuralType,
                                        child_path(path, "type_name"));
        const auto* schema = registry_.find(domain, type_name);
        if (!schema)
            return fail<RewriteSuccess>(PersistenceErrorCode::UnsupportedStructuralType,
                                        child_path(path, "type_name"));
        if (source_version.value() > schema->current_version ||
            target->version > schema->current_version)
            return fail<RewriteSuccess>(PersistenceErrorCode::UnsupportedSchemaVersion,
                                        child_path(path, "version"));

        if (source_version.value() != target->version) {
            auto source = canonicalize_json(envelope);
            if (!source)
                return runtime::Result<RewriteSuccess, PersistenceError>(
                    runtime::Err(source.error()));
            DecodeLimits limits;
            limits.max_input_bytes = maximum_;
            auto migrated = registry_.migrate(domain, type_name, source_version.value(),
                                              target->version, source.value(), limits);
            if (!migrated) {
                auto error = migrated.error();
                if (error.code == PersistenceErrorCode::LimitExceeded)
                    error.code = PersistenceErrorCode::OutputLimitExceeded;
                error.path = path;
                return runtime::Result<RewriteSuccess, PersistenceError>(
                    runtime::Err(std::move(error)));
            }
            auto parsed = parse_json(migrated.value(), limits);
            if (!parsed)
                return runtime::Result<RewriteSuccess, PersistenceError>(
                    runtime::Err(parsed.error()));
            auto canonical_migrated = canonicalize_json(parsed.value()->root());
            if (!canonical_migrated)
                return runtime::Result<RewriteSuccess, PersistenceError>(
                    runtime::Err(canonical_migrated.error()));
            if (source.value().size() > current_size_)
                return fail<RewriteSuccess>(PersistenceErrorCode::InvalidSchema, path);
            const auto without_source = current_size_ - source.value().size();
            if (canonical_migrated.value().size() > maximum_ - without_source)
                return fail<RewriteSuccess>(PersistenceErrorCode::OutputLimitExceeded, path,
                                            without_source + canonical_migrated.value().size(),
                                            maximum_);
            current_size_ = without_source + canonical_migrated.value().size();
            envelope = parsed.value()->root();
            data = member(envelope, "data");
            if (!data)
                return fail<RewriteSuccess>(PersistenceErrorCode::MigrationFailed, path);
        }

        return rewrite_children(type_name, *data, child_path(path, "data"));
    }

  private:
    runtime::Result<RewriteSuccess, PersistenceError>
    rewrite_array(JsonValue& data, std::string_view field, SchemaDomain domain,
                  std::string_view expected_type, const std::string& path) const {
        auto* values = member(data, field);
        if (!values)
            return runtime::Ok(RewriteSuccess{});
        if (values->kind != JsonValue::Kind::Array)
            return fail<RewriteSuccess>(PersistenceErrorCode::InvalidSchema,
                                        child_path(path, field));
        for (std::size_t index = 0; index < values->array.size(); ++index) {
            auto rewritten = rewrite(values->array[index], domain, expected_type,
                                     child_path(child_path(path, field), std::to_string(index)));
            if (!rewritten)
                return rewritten;
        }
        return runtime::Ok(RewriteSuccess{});
    }

    runtime::Result<RewriteSuccess, PersistenceError>
    rewrite_member(JsonValue& data, std::string_view field, SchemaDomain domain,
                   std::string_view expected_type, const std::string& path) const {
        auto* value = member(data, field);
        if (!value)
            return fail<RewriteSuccess>(PersistenceErrorCode::InvalidSchema,
                                        child_path(path, field));
        return rewrite(*value, domain, expected_type, child_path(path, field));
    }

    bool supports_identity_kind(std::string_view kind) const noexcept {
        if (kind == "project" || kind == "asset" || kind == "sequence" || kind == "track" ||
            kind == "clip" || kind == "note")
            return true;
        if (kind == "device_placement")
            return release_.find(SchemaDomain::Document, "pulp.timeline.device_placement") !=
                   nullptr;
        if (kind == "automation_lane" || kind == "automation_point")
            return release_.find(SchemaDomain::Document, "pulp.timeline.automation_lane") !=
                   nullptr;
        if (kind == "take_lane" || kind == "take")
            return release_.find(SchemaDomain::Document, "pulp.timeline.take_lane") != nullptr;
        return false;
    }

    runtime::Result<RewriteSuccess, PersistenceError>
    rewrite_identities(JsonValue& data, const std::string& path) const {
        auto* identities = member(data, "identities");
        if (!identities)
            return runtime::Ok(RewriteSuccess{});
        auto* next_id_value = member(data, "next_item_id");
        if (identities->kind != JsonValue::Kind::Array || !next_id_value)
            return fail<RewriteSuccess>(PersistenceErrorCode::InvalidSchema,
                                        child_path(path, "identities"));
        auto next_id = parse_canonical_u64_string(*next_id_value, child_path(path, "next_item_id"));
        if (!next_id)
            return runtime::Result<RewriteSuccess, PersistenceError>(runtime::Err(next_id.error()));

        std::size_t retained_count = 0;
        for (std::size_t index = 0; index < identities->array.size(); ++index) {
            auto& identity = identities->array[index];
            const auto identity_path =
                child_path(child_path(path, "identities"), std::to_string(index));
            auto* kind = member(identity, "kind");
            auto* active = member(identity, "active");
            auto* item = member(identity, "id");
            if (!kind || kind->kind != JsonValue::Kind::String || !active ||
                active->kind != JsonValue::Kind::Boolean || !item)
                return fail<RewriteSuccess>(PersistenceErrorCode::InvalidSchema, identity_path);
            if (supports_identity_kind(kind->scalar)) {
                if (retained_count != index)
                    identities->array[retained_count] = std::move(identity);
                ++retained_count;
                continue;
            }
            if (active->boolean)
                return fail<RewriteSuccess>(PersistenceErrorCode::UnsupportedStructuralType,
                                            child_path(identity_path, "kind"));
            auto item_id = parse_canonical_u64_string(*item, child_path(identity_path, "id"));
            if (!item_id)
                return runtime::Result<RewriteSuccess, PersistenceError>(
                    runtime::Err(item_id.error()));
            if (item_id.value() >= next_id.value())
                return fail<RewriteSuccess>(PersistenceErrorCode::InvalidSchema,
                                            child_path(identity_path, "id"));
        }
        identities->array.resize(retained_count);
        return runtime::Ok(RewriteSuccess{});
    }

    runtime::Result<RewriteSuccess, PersistenceError>
    rewrite_children(std::string_view type_name, JsonValue& data, const std::string& path) const {
        if (type_name == "pulp.timeline.project") {
            auto assets =
                rewrite_array(data, "assets", SchemaDomain::Document, "pulp.timeline.asset", path);
            if (!assets)
                return assets;
            auto sequences = rewrite_array(data, "sequences", SchemaDomain::Document,
                                           "pulp.timeline.sequence", path);
            if (!sequences)
                return sequences;
            return rewrite_identities(data, path);
        }
        if (type_name == "pulp.timeline.asset")
            return rewrite_array(data, "representations", SchemaDomain::AssetRepresentation,
                                 "pulp.timeline.asset_representation", path);
        if (type_name == "pulp.timeline.sequence")
            return rewrite_array(data, "tracks", SchemaDomain::Document, "pulp.timeline.track",
                                 path);
        if (type_name == "pulp.timeline.track") {
            auto automation = rewrite_array(data, "automation_lanes", SchemaDomain::Document,
                                            "pulp.timeline.automation_lane", path);
            if (!automation)
                return automation;
            auto clips =
                rewrite_array(data, "clips", SchemaDomain::Document, "pulp.timeline.clip", path);
            if (!clips)
                return clips;
            auto devices = rewrite_array(data, "device_chain", SchemaDomain::Document,
                                         "pulp.timeline.device_placement", path);
            if (!devices)
                return devices;
            return rewrite_array(data, "take_lanes", SchemaDomain::Document,
                                 "pulp.timeline.take_lane", path);
        }
        if (type_name == "pulp.timeline.automation_lane")
            return rewrite_member(data, "target", SchemaDomain::Document, {}, path);
        if (type_name == "pulp.timeline.take_lane")
            return rewrite_array(data, "takes", SchemaDomain::Document, "pulp.timeline.take", path);
        if (type_name == "pulp.timeline.clip")
            return rewrite_member(data, "content", SchemaDomain::Content, {}, path);
        return runtime::Ok(RewriteSuccess{});
    }

    const SchemaRegistry& registry_;
    const SchemaReleaseMap& release_;
    std::size_t maximum_;
    mutable std::size_t current_size_;
};

} // namespace

runtime::Result<SerializedSnapshot, PersistenceError>
serialize_project_for_release(const Project& project, const SchemaRegistry& registry,
                              const SchemaReleaseMap& release, const SerializeOptions& options) {
    auto valid = validate_release_map(release, registry);
    if (!valid)
        return runtime::Result<SerializedSnapshot, PersistenceError>(runtime::Err(valid.error()));
    auto current = serialize_project(project, registry, options);
    if (!current)
        return current;

    DecodeLimits limits;
    limits.max_input_bytes = options.max_output_bytes;
    auto parsed = parse_json(current.value().json, limits);
    if (!parsed)
        return runtime::Result<SerializedSnapshot, PersistenceError>(runtime::Err(parsed.error()));
    auto root = parsed.value()->root();
    ReleaseRewriter rewriter(registry, release, options.max_output_bytes,
                             current.value().json.size());
    auto rewritten = rewriter.rewrite(root, SchemaDomain::Document, "pulp.timeline.project", "/");
    if (!rewritten)
        return runtime::Result<SerializedSnapshot, PersistenceError>(
            runtime::Err(rewritten.error()));
    auto json = canonicalize_json(root);
    if (!json)
        return runtime::Result<SerializedSnapshot, PersistenceError>(runtime::Err(json.error()));
    if (json.value().size() > options.max_output_bytes)
        return fail<SerializedSnapshot>(PersistenceErrorCode::OutputLimitExceeded, "/",
                                        json.value().size(), options.max_output_bytes);
    return runtime::Ok(SerializedSnapshot{std::move(json).value(), false});
}

} // namespace pulp::timeline
