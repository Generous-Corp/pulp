# PulpSdkProvenance.cmake — canonical installed-SDK distribution contract.
#
# Released SDKs predating provenance files remain distribution-eligible.
# A present marker is authoritative and must match the development schema
# exactly; malformed or unknown markers fail closed during find_package(Pulp).

set(PULP_SDK_PROVENANCE_KIND "release")
set(PULP_SDK_DEVELOPMENT FALSE)
set(PULP_SDK_DISTRIBUTION_ELIGIBLE TRUE)
set(PULP_SDK_SOURCE_GIT_SHA "")

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
    if(_pulp_sdk_schema_error OR _pulp_sdk_kind_error OR
       _pulp_sdk_eligible_error OR _pulp_sdk_sha_error)
        message(FATAL_ERROR
            "Pulp SDK provenance is malformed at ${_pulp_sdk_provenance}; "
            "refusing to guess whether this SDK can be distributed.")
    endif()
    if(NOT _pulp_sdk_schema STREQUAL "pulp.sdk-provenance.v1" OR
       NOT _pulp_sdk_kind STREQUAL "development" OR
       _pulp_sdk_eligible)
        message(FATAL_ERROR
            "Pulp SDK provenance at ${_pulp_sdk_provenance} has an unknown "
            "or unsafe contract; refusing to consume it.")
    endif()
    set(PULP_SDK_PROVENANCE_KIND "${_pulp_sdk_kind}")
    set(PULP_SDK_DEVELOPMENT TRUE)
    set(PULP_SDK_DISTRIBUTION_ELIGIBLE FALSE)
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
