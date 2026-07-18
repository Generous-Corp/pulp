#pragma once

#include <pulp/audio/sample_heritage.hpp>

#include <string>
#include <string_view>

namespace pulp::audio {

/// Strict errors for the versioned sample-heritage JSON boundary.
enum class SampleHeritageJsonStatus : std::uint8_t {
    Ok,
    InvalidJson,
    RootNotObject,
    UnknownField,
    DuplicateField,
    MissingField,
    WrongType,
    InvalidEnum,
    NumberOutOfRange,
    ProfileValidationFailed,
};

struct SampleHeritageJsonParseResult {
    SampleHeritageJsonStatus status = SampleHeritageJsonStatus::InvalidJson;
    std::string field_path;
    SampleHeritageProfileStatus profile_status =
        SampleHeritageProfileStatus::UnsupportedSchemaVersion;
    SampleHeritageProfile profile;

    bool valid() const noexcept { return status == SampleHeritageJsonStatus::Ok; }
};

struct SampleHeritageJsonWriteResult {
    SampleHeritageJsonStatus status =
        SampleHeritageJsonStatus::ProfileValidationFailed;
    SampleHeritageProfileStatus profile_status =
        SampleHeritageProfileStatus::UnsupportedSchemaVersion;
    std::string json;

    bool valid() const noexcept { return status == SampleHeritageJsonStatus::Ok; }
};

struct SampleHeritageRuntimeStateJsonParseResult {
    SampleHeritageJsonStatus status = SampleHeritageJsonStatus::InvalidJson;
    std::string field_path;
    SampleHeritageRuntimeStateStatus runtime_status =
        SampleHeritageRuntimeStateStatus::InvalidStageLayout;
    SampleHeritageRuntimeState state;

    bool valid() const noexcept { return status == SampleHeritageJsonStatus::Ok; }
};

struct SampleHeritageRuntimeStateJsonWriteResult {
    SampleHeritageJsonStatus status =
        SampleHeritageJsonStatus::ProfileValidationFailed;
    SampleHeritageRuntimeStateStatus runtime_status =
        SampleHeritageRuntimeStateStatus::InvalidStageLayout;
    std::string json;

    bool valid() const noexcept { return status == SampleHeritageJsonStatus::Ok; }
};

/// Parses the complete schema-v1 contract. This is an allocating, off-audio-thread API.
SampleHeritageJsonParseResult parse_sample_heritage_profile_json(
    std::string_view json);

/// Validates and writes one deterministic, whitespace-free schema-v1 representation.
/// This is an allocating, off-audio-thread API.
SampleHeritageJsonWriteResult write_sample_heritage_profile_json(
    const SampleHeritageProfile& profile);

/// Strict canonical persistence for quiescent runtime continuation snapshots.
SampleHeritageRuntimeStateJsonParseResult parse_sample_heritage_runtime_state_json(
    std::string_view json);
SampleHeritageRuntimeStateJsonWriteResult write_sample_heritage_runtime_state_json(
    const SampleHeritageRuntimeState& state);

}  // namespace pulp::audio
