#include "support/timeline_persistence_test_support.hpp"

TEST_CASE("Timeline persistence requires the complete compatible structural registry") {
    const auto project = project_with();
    const auto snapshot = take(serialize_project(project, builtins())).json;
    constexpr std::string_view structural_types[] = {
        "pulp.timeline.project",
        "pulp.timeline.asset",
        "pulp.timeline.asset_representation",
        "pulp.timeline.sequence",
        "pulp.timeline.track",
        "pulp.timeline.device_placement",
        "pulp.timeline.clip",
        "pulp.timeline.content.empty",
        "pulp.timeline.content.media",
        "pulp.timeline.content.notes",
    };
    for (const auto type_name : structural_types) {
        const auto incomplete = structurally_modified_registry(type_name);
        auto encode_missing = serialize_project(project, incomplete);
        REQUIRE_FALSE(encode_missing.has_value());
        REQUIRE(encode_missing.error().code == PersistenceErrorCode::UnsupportedStructuralType);
        auto decode_missing = deserialize_project(snapshot, incomplete);
        REQUIRE_FALSE(decode_missing.has_value());
        REQUIRE(decode_missing.error().code == PersistenceErrorCode::UnsupportedStructuralType);

        const auto conflicting = structurally_modified_registry({}, type_name);
        auto encode_conflict = serialize_project(project, conflicting);
        REQUIRE_FALSE(encode_conflict.has_value());
        REQUIRE(encode_conflict.error().code == PersistenceErrorCode::UnsupportedSchemaVersion);
        auto decode_conflict = deserialize_project(snapshot, conflicting);
        REQUIRE_FALSE(decode_conflict.has_value());
        REQUIRE(decode_conflict.error().code == PersistenceErrorCode::UnsupportedSchemaVersion);

        const auto incompatible_fields = structurally_modified_registry({}, {}, type_name);
        auto encode_fields = serialize_project(project, incompatible_fields);
        REQUIRE_FALSE(encode_fields.has_value());
        REQUIRE(encode_fields.error().code == PersistenceErrorCode::InvalidSchema);
        auto decode_fields = deserialize_project(snapshot, incompatible_fields);
        REQUIRE_FALSE(decode_fields.has_value());
        REQUIRE(decode_fields.error().code == PersistenceErrorCode::InvalidSchema);
    }
    for (const auto mutation : {FieldMutation::Kind, FieldMutation::Reference}) {
        const auto incompatible =
            structurally_modified_registry({}, {}, "pulp.timeline.project", mutation);
        auto encode = serialize_project(project, incompatible);
        REQUIRE_FALSE(encode.has_value());
        REQUIRE(encode.error().code == PersistenceErrorCode::InvalidSchema);
        auto decode = deserialize_project(snapshot, incompatible);
        REQUIRE_FALSE(decode.has_value());
        REQUIRE(decode.error().code == PersistenceErrorCode::InvalidSchema);
    }
}
