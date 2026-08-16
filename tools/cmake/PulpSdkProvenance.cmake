# PulpSdkProvenance.cmake — canonical installed-SDK distribution contract.
#
# Marker-free SDKs remain usable for local compatibility, but are never
# distribution-eligible. A present marker is authoritative; malformed or
# unknown release/development polarity fails closed during find_package(Pulp).

set(PULP_SDK_PROVENANCE_KIND "unmarked")
set(PULP_SDK_DEVELOPMENT FALSE)
set(PULP_SDK_DISTRIBUTION_ELIGIBLE FALSE)
set(PULP_SDK_SOURCE_GIT_SHA "")
set(PULP_SDK_PLATFORM "")
set(PULP_SDK_AUDIO_PROBES_ENABLED "")
set(PULP_SDK_INSPECTOR_ENABLED "")

set(_pulp_sdk_provenance "${PULP_SDK_DIR}/sdk-provenance.json")
if(EXISTS "${_pulp_sdk_provenance}")
    file(READ "${_pulp_sdk_provenance}" _pulp_sdk_provenance_json)
    string(JSON _pulp_sdk_schema ERROR_VARIABLE _pulp_sdk_schema_error
           GET "${_pulp_sdk_provenance_json}" schema)
    string(JSON _pulp_sdk_kind ERROR_VARIABLE _pulp_sdk_kind_error
           GET "${_pulp_sdk_provenance_json}" kind)
    string(JSON _pulp_sdk_eligible ERROR_VARIABLE _pulp_sdk_eligible_error
           GET "${_pulp_sdk_provenance_json}" distribution_eligible)
    string(JSON _pulp_sdk_source_sha ERROR_VARIABLE _pulp_sdk_sha_error
           GET "${_pulp_sdk_provenance_json}" source_git_sha)
    string(JSON _pulp_sdk_schema_type ERROR_VARIABLE _pulp_sdk_schema_type_error
           TYPE "${_pulp_sdk_provenance_json}" schema)
    string(JSON _pulp_sdk_kind_type ERROR_VARIABLE _pulp_sdk_kind_type_error
           TYPE "${_pulp_sdk_provenance_json}" kind)
    string(JSON _pulp_sdk_eligible_type ERROR_VARIABLE _pulp_sdk_eligible_type_error
           TYPE "${_pulp_sdk_provenance_json}" distribution_eligible)
    string(JSON _pulp_sdk_sha_type ERROR_VARIABLE _pulp_sdk_sha_type_error
           TYPE "${_pulp_sdk_provenance_json}" source_git_sha)
    if(_pulp_sdk_schema_error OR _pulp_sdk_kind_error OR
       _pulp_sdk_eligible_error OR _pulp_sdk_sha_error OR
       _pulp_sdk_schema_type_error OR _pulp_sdk_kind_type_error OR
       _pulp_sdk_eligible_type_error OR _pulp_sdk_sha_type_error)
        message(FATAL_ERROR
            "Pulp SDK provenance is malformed at ${_pulp_sdk_provenance}; "
            "refusing to guess whether this SDK can be distributed.")
    endif()
    string(LENGTH "${_pulp_sdk_source_sha}" _pulp_sdk_source_sha_length)
    if(NOT _pulp_sdk_schema_type STREQUAL "STRING" OR
       NOT _pulp_sdk_kind_type STREQUAL "STRING" OR
       NOT _pulp_sdk_eligible_type STREQUAL "BOOLEAN" OR
       NOT _pulp_sdk_sha_type STREQUAL "STRING" OR
       NOT _pulp_sdk_source_sha MATCHES "^[0-9a-f]+$" OR
       NOT _pulp_sdk_source_sha_length EQUAL 40 OR
       NOT _pulp_sdk_schema STREQUAL "pulp.sdk-provenance.v1")
        message(FATAL_ERROR
            "Pulp SDK provenance at ${_pulp_sdk_provenance} has an unknown "
            "or unsafe contract; refusing to consume it.")
    endif()

    if(_pulp_sdk_kind STREQUAL "development")
        if(_pulp_sdk_eligible)
            message(FATAL_ERROR
                "Pulp SDK provenance at ${_pulp_sdk_provenance} has an unknown "
                "or unsafe contract; refusing to consume it.")
        endif()
        set(PULP_SDK_DEVELOPMENT TRUE)
    elseif(_pulp_sdk_kind STREQUAL "release")
        string(JSON _pulp_sdk_profile ERROR_VARIABLE _pulp_sdk_profile_error
               GET "${_pulp_sdk_provenance_json}" profile)
        string(JSON _pulp_sdk_dirty ERROR_VARIABLE _pulp_sdk_dirty_error
               GET "${_pulp_sdk_provenance_json}" source_git_dirty)
        string(JSON _pulp_sdk_build_type ERROR_VARIABLE _pulp_sdk_build_type_error
               GET "${_pulp_sdk_provenance_json}" build_type)
        string(JSON _pulp_sdk_audio_probes ERROR_VARIABLE _pulp_sdk_audio_probes_error
               GET "${_pulp_sdk_provenance_json}" features audio_probes)
        string(JSON _pulp_sdk_inspector ERROR_VARIABLE _pulp_sdk_inspector_error
               GET "${_pulp_sdk_provenance_json}" features inspector)
        string(JSON _pulp_sdk_version ERROR_VARIABLE _pulp_sdk_version_error
               GET "${_pulp_sdk_provenance_json}" sdk_version)
        string(JSON _pulp_sdk_source_ref ERROR_VARIABLE _pulp_sdk_source_ref_error
               GET "${_pulp_sdk_provenance_json}" source_git_ref)
        string(JSON _pulp_sdk_platform ERROR_VARIABLE _pulp_sdk_platform_error
               GET "${_pulp_sdk_provenance_json}" platform)
        string(JSON _pulp_sdk_profile_type ERROR_VARIABLE _pulp_sdk_profile_type_error
               TYPE "${_pulp_sdk_provenance_json}" profile)
        string(JSON _pulp_sdk_dirty_type ERROR_VARIABLE _pulp_sdk_dirty_type_error
               TYPE "${_pulp_sdk_provenance_json}" source_git_dirty)
        string(JSON _pulp_sdk_build_type_type ERROR_VARIABLE _pulp_sdk_build_type_type_error
               TYPE "${_pulp_sdk_provenance_json}" build_type)
        string(JSON _pulp_sdk_audio_probes_type
               ERROR_VARIABLE _pulp_sdk_audio_probes_type_error
               TYPE "${_pulp_sdk_provenance_json}" features audio_probes)
        string(JSON _pulp_sdk_inspector_type ERROR_VARIABLE _pulp_sdk_inspector_type_error
               TYPE "${_pulp_sdk_provenance_json}" features inspector)
        string(JSON _pulp_sdk_version_type ERROR_VARIABLE _pulp_sdk_version_type_error
               TYPE "${_pulp_sdk_provenance_json}" sdk_version)
        string(JSON _pulp_sdk_source_ref_type ERROR_VARIABLE _pulp_sdk_source_ref_type_error
               TYPE "${_pulp_sdk_provenance_json}" source_git_ref)
        string(JSON _pulp_sdk_platform_type ERROR_VARIABLE _pulp_sdk_platform_type_error
               TYPE "${_pulp_sdk_provenance_json}" platform)
        set(_pulp_sdk_expected_inspector TRUE)
        if(_pulp_sdk_version VERSION_LESS "0.772.0")
            set(_pulp_sdk_expected_inspector FALSE)
        endif()
        if(_pulp_sdk_profile_error OR _pulp_sdk_dirty_error OR
           _pulp_sdk_build_type_error OR _pulp_sdk_audio_probes_error OR
           _pulp_sdk_inspector_error OR _pulp_sdk_version_error OR
           _pulp_sdk_source_ref_error OR _pulp_sdk_platform_error OR
           _pulp_sdk_profile_type_error OR _pulp_sdk_dirty_type_error OR
           _pulp_sdk_build_type_type_error OR _pulp_sdk_audio_probes_type_error OR
           _pulp_sdk_inspector_type_error OR _pulp_sdk_version_type_error OR
           _pulp_sdk_source_ref_type_error OR _pulp_sdk_platform_type_error OR
           NOT _pulp_sdk_profile_type STREQUAL "STRING" OR
           NOT _pulp_sdk_dirty_type STREQUAL "BOOLEAN" OR
           NOT _pulp_sdk_build_type_type STREQUAL "STRING" OR
           NOT _pulp_sdk_audio_probes_type STREQUAL "BOOLEAN" OR
           NOT _pulp_sdk_inspector_type STREQUAL "BOOLEAN" OR
           NOT _pulp_sdk_version_type STREQUAL "STRING" OR
           NOT _pulp_sdk_source_ref_type STREQUAL "STRING" OR
           NOT _pulp_sdk_platform_type STREQUAL "STRING" OR
           NOT _pulp_sdk_profile STREQUAL "official-release" OR
           NOT _pulp_sdk_eligible OR _pulp_sdk_dirty OR
           NOT _pulp_sdk_build_type STREQUAL "Release" OR
           _pulp_sdk_audio_probes OR
           (_pulp_sdk_expected_inspector AND NOT _pulp_sdk_inspector) OR
           (NOT _pulp_sdk_expected_inspector AND _pulp_sdk_inspector) OR
           NOT _pulp_sdk_version MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$" OR
           NOT _pulp_sdk_source_ref STREQUAL "v${_pulp_sdk_version}" OR
           NOT _pulp_sdk_platform MATCHES "^(darwin|linux|windows)-(arm64|x64)$" OR
           (DEFINED PULP_VERSION AND NOT _pulp_sdk_version STREQUAL PULP_VERSION))
            message(FATAL_ERROR
                "Pulp SDK provenance at ${_pulp_sdk_provenance} has an unknown "
                "or unsafe release contract; refusing to consume it.")
        endif()
        set(PULP_SDK_DISTRIBUTION_ELIGIBLE TRUE)
        set(PULP_SDK_PLATFORM "${_pulp_sdk_platform}")
        set(PULP_SDK_AUDIO_PROBES_ENABLED FALSE)
        set(PULP_SDK_INSPECTOR_ENABLED "${_pulp_sdk_inspector}")

        # WidgetBridge owns callback-lifetime state in its public class layout,
        # while event dispatch is implemented by pulp-view-script. Mixing a
        # stale installed header with a fresh archive can therefore link cleanly
        # and crash on the first click. Official SDKs bind those two files in
        # the release provenance marker and fail before any consumer compiles.
        if(NOT _pulp_sdk_version VERSION_LESS "0.807.0" AND
           _pulp_sdk_platform MATCHES "^windows-")
            set(_pulp_sdk_view_script_path "lib/pulp-view-script.lib")
        elseif(NOT _pulp_sdk_version VERSION_LESS "0.807.0")
            set(_pulp_sdk_view_script_path "lib/libpulp-view-script.a")
        endif()
        if(NOT _pulp_sdk_version VERSION_LESS "0.807.0")
            set(_pulp_sdk_integrity_paths
                "include/pulp/view/widget_bridge.hpp"
                "${_pulp_sdk_view_script_path}")
            string(JSON _pulp_sdk_integrity_schema ERROR_VARIABLE _pulp_sdk_integrity_schema_error
                   GET "${_pulp_sdk_provenance_json}" integrity schema)
            string(JSON _pulp_sdk_integrity_algorithm ERROR_VARIABLE _pulp_sdk_integrity_algorithm_error
                   GET "${_pulp_sdk_provenance_json}" integrity algorithm)
            if(_pulp_sdk_integrity_schema_error OR _pulp_sdk_integrity_algorithm_error OR
               NOT _pulp_sdk_integrity_schema STREQUAL "pulp.sdk-integrity.v1" OR
               NOT _pulp_sdk_integrity_algorithm STREQUAL "sha256")
                message(FATAL_ERROR
                    "Pulp SDK provenance at ${_pulp_sdk_provenance} has no valid "
                    "WidgetBridge coherence contract; refusing to consume it.")
            endif()
            foreach(_pulp_sdk_integrity_path IN LISTS _pulp_sdk_integrity_paths)
                string(JSON _pulp_sdk_expected_hash ERROR_VARIABLE _pulp_sdk_hash_error
                       GET "${_pulp_sdk_provenance_json}" integrity files
                       "${_pulp_sdk_integrity_path}")
                set(_pulp_sdk_integrity_file "${PULP_SDK_DIR}/${_pulp_sdk_integrity_path}")
                if(_pulp_sdk_hash_error OR
                   NOT _pulp_sdk_expected_hash MATCHES "^[0-9a-f]+$" OR
                   NOT EXISTS "${_pulp_sdk_integrity_file}")
                    message(FATAL_ERROR
                        "Pulp SDK coherence member ${_pulp_sdk_integrity_path} is "
                        "missing or unauthenticated; refusing to consume it.")
                endif()
                string(LENGTH "${_pulp_sdk_expected_hash}" _pulp_sdk_expected_hash_length)
                file(SHA256 "${_pulp_sdk_integrity_file}" _pulp_sdk_actual_hash)
                if(NOT _pulp_sdk_expected_hash_length EQUAL 64 OR
                   NOT _pulp_sdk_actual_hash STREQUAL _pulp_sdk_expected_hash)
                    message(FATAL_ERROR
                        "Pulp SDK coherence mismatch for ${_pulp_sdk_integrity_path}; "
                        "reinstall the complete SDK instead of swapping headers or libraries.")
                endif()
            endforeach()
        endif()
    else()
        message(FATAL_ERROR
            "Pulp SDK provenance at ${_pulp_sdk_provenance} has an unknown "
            "or unsafe contract; refusing to consume it.")
    endif()
    set(PULP_SDK_PROVENANCE_KIND "${_pulp_sdk_kind}")
    set(PULP_SDK_SOURCE_GIT_SHA "${_pulp_sdk_source_sha}")
endif()

# Cache the resolved contract so release tools inspecting the consumer build
# tree can reject development SDKs without rediscovering the package path.
set(PULP_SDK_PROVENANCE_KIND "${PULP_SDK_PROVENANCE_KIND}" CACHE INTERNAL
    "Resolved Pulp SDK provenance kind" FORCE)
set(PULP_SDK_DEVELOPMENT "${PULP_SDK_DEVELOPMENT}" CACHE INTERNAL
    "Whether the selected Pulp SDK is development-only" FORCE)
set(PULP_SDK_DISTRIBUTION_ELIGIBLE "${PULP_SDK_DISTRIBUTION_ELIGIBLE}" CACHE INTERNAL
    "Whether artifacts built against the selected Pulp SDK may be distributed" FORCE)
set(PULP_SDK_SOURCE_GIT_SHA "${PULP_SDK_SOURCE_GIT_SHA}" CACHE INTERNAL
    "Source commit recorded by the selected Pulp SDK" FORCE)
set(PULP_SDK_PLATFORM "${PULP_SDK_PLATFORM}" CACHE INTERNAL
    "Release platform recorded by the selected Pulp SDK" FORCE)
set(PULP_SDK_AUDIO_PROBES_ENABLED "${PULP_SDK_AUDIO_PROBES_ENABLED}" CACHE INTERNAL
    "Whether the selected SDK was built with audio probes" FORCE)
set(PULP_SDK_INSPECTOR_ENABLED "${PULP_SDK_INSPECTOR_ENABLED}" CACHE INTERNAL
    "Whether the selected SDK was built with the inspector" FORCE)

unset(_pulp_sdk_provenance)
unset(_pulp_sdk_provenance_json)
unset(_pulp_sdk_schema)
unset(_pulp_sdk_schema_error)
unset(_pulp_sdk_kind)
unset(_pulp_sdk_kind_error)
unset(_pulp_sdk_eligible)
unset(_pulp_sdk_eligible_error)
unset(_pulp_sdk_source_sha)
unset(_pulp_sdk_sha_error)
unset(_pulp_sdk_schema_type)
unset(_pulp_sdk_schema_type_error)
unset(_pulp_sdk_kind_type)
unset(_pulp_sdk_kind_type_error)
unset(_pulp_sdk_eligible_type)
unset(_pulp_sdk_eligible_type_error)
unset(_pulp_sdk_sha_type)
unset(_pulp_sdk_sha_type_error)
unset(_pulp_sdk_source_sha_length)
unset(_pulp_sdk_profile)
unset(_pulp_sdk_profile_error)
unset(_pulp_sdk_dirty)
unset(_pulp_sdk_dirty_error)
unset(_pulp_sdk_build_type)
unset(_pulp_sdk_build_type_error)
unset(_pulp_sdk_audio_probes)
unset(_pulp_sdk_audio_probes_error)
unset(_pulp_sdk_inspector)
unset(_pulp_sdk_inspector_error)
unset(_pulp_sdk_version)
unset(_pulp_sdk_version_error)
unset(_pulp_sdk_source_ref)
unset(_pulp_sdk_source_ref_error)
unset(_pulp_sdk_platform)
unset(_pulp_sdk_platform_error)
unset(_pulp_sdk_profile_type)
unset(_pulp_sdk_profile_type_error)
unset(_pulp_sdk_dirty_type)
unset(_pulp_sdk_dirty_type_error)
unset(_pulp_sdk_build_type_type)
unset(_pulp_sdk_build_type_type_error)
unset(_pulp_sdk_audio_probes_type)
unset(_pulp_sdk_audio_probes_type_error)
unset(_pulp_sdk_inspector_type)
unset(_pulp_sdk_inspector_type_error)
unset(_pulp_sdk_expected_inspector)
unset(_pulp_sdk_version_type)
unset(_pulp_sdk_version_type_error)
unset(_pulp_sdk_source_ref_type)
unset(_pulp_sdk_source_ref_type_error)
unset(_pulp_sdk_platform_type)
unset(_pulp_sdk_platform_type_error)
unset(_pulp_sdk_view_script_path)
unset(_pulp_sdk_integrity_paths)
unset(_pulp_sdk_integrity_schema)
unset(_pulp_sdk_integrity_schema_error)
unset(_pulp_sdk_integrity_algorithm)
unset(_pulp_sdk_integrity_algorithm_error)
unset(_pulp_sdk_integrity_path)
unset(_pulp_sdk_expected_hash)
unset(_pulp_sdk_hash_error)
unset(_pulp_sdk_integrity_file)
unset(_pulp_sdk_expected_hash_length)
unset(_pulp_sdk_actual_hash)
