# Read and validate the repository's Vellum release lock.
#
# What each lock field binds:
#   tag, sha            the Vellum release and its commit. The FetchContent source
#                       build of gpu-runtime is pinned by this commit, not by an
#                       archive, so it is covered by `sha` alone.
#   gpu_runtime_digest  sha256 of the prebuilt vellum-gpu-runtime archive. Enforced
#                       for PULP_GPU_RUNTIME_PREFIX (PulpGpuRuntime.cmake requires
#                       PULP_GPU_RUNTIME_ARCHIVE and fails closed without it).
#   sdk_digest          sha256 of the prebuilt Vellum SDK archive. Pulp itself
#                       ingests no Vellum SDK archive (the SDK consumer lane
#                       deliberately tracks Vellum main), so it is enforced only
#                       when a caller supplies PULP_VELLUM_SDK_ARCHIVE.
#   render_toolchain    lock_digest is sha256 of the exact bytes of
#                       gpu-runtime/renderer-lock.json at vellum.sha. Configure only
#                       checks its shape; tools/deps/audit.py --strict reads that file
#                       at the pinned commit and fails unless the digest matches.
set(PULP_VELLUM_LOCK_FILE "${CMAKE_CURRENT_LIST_DIR}/../../pulp.lock"
    CACHE FILEPATH "Pulp Vellum lock file")
if(NOT EXISTS "${PULP_VELLUM_LOCK_FILE}")
    message(FATAL_ERROR "Pulp: missing pulp.lock at ${PULP_VELLUM_LOCK_FILE}")
endif()
file(READ "${PULP_VELLUM_LOCK_FILE}" _pulp_vellum_lock)
string(JSON _pulp_lock_schema ERROR_VARIABLE _pulp_lock_error GET "${_pulp_vellum_lock}" schema)
if(_pulp_lock_error OR NOT _pulp_lock_schema STREQUAL "pulp.lock.v1")
    message(FATAL_ERROR "Pulp: pulp.lock has an invalid schema")
endif()
foreach(_field tag sha sdk_digest gpu_runtime_digest)
    string(JSON _value ERROR_VARIABLE _error GET "${_pulp_vellum_lock}" vellum ${_field})
    if(_error OR _value STREQUAL "")
        message(FATAL_ERROR "Pulp: pulp.lock is missing vellum.${_field}")
    endif()
    string(LENGTH "${_value}" _value_length)
    # The same tag form tools/deps/pulp_lock.py requires.
    if(_field STREQUAL "tag" AND NOT _value MATCHES "^v[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "Pulp: pulp.lock vellum.tag is not a release tag such as v0.2.0")
    endif()
    if(_field STREQUAL "sha" AND (NOT _value_length EQUAL 40 OR NOT _value MATCHES "^[0-9a-f]+$"))
        message(FATAL_ERROR "Pulp: pulp.lock vellum.sha is not a lowercase 40-hex SHA")
    elseif((_field STREQUAL "sdk_digest" OR _field STREQUAL "gpu_runtime_digest") AND (NOT _value_length EQUAL 64 OR NOT _value MATCHES "^[0-9a-f]+$"))
        message(FATAL_ERROR "Pulp: pulp.lock vellum.${_field} is not a lowercase 64-hex digest")
    endif()
    set(PULP_VELLUM_${_field} "${_value}")
endforeach()
string(JSON PULP_VELLUM_RENDER_LOCK_DIGEST ERROR_VARIABLE _render_error GET
    "${_pulp_vellum_lock}" render_toolchain lock_digest)
string(LENGTH "${PULP_VELLUM_RENDER_LOCK_DIGEST}" _render_digest_length)
if(_render_error OR NOT _render_digest_length EQUAL 64 OR NOT PULP_VELLUM_RENDER_LOCK_DIGEST MATCHES "^[0-9a-f]+$")
    message(FATAL_ERROR "Pulp: pulp.lock is missing a valid render_toolchain.lock_digest")
endif()
if(DEFINED PULP_VELLUM_TAG_SHA AND NOT PULP_VELLUM_TAG_SHA STREQUAL PULP_VELLUM_sha)
    message(FATAL_ERROR "Pulp: pulp.lock tag does not resolve to its pinned sha")
endif()
if(DEFINED PULP_VELLUM_SDK_ARCHIVE AND NOT PULP_VELLUM_SDK_ARCHIVE STREQUAL "")
    if(NOT EXISTS "${PULP_VELLUM_SDK_ARCHIVE}")
        message(FATAL_ERROR "Pulp: SDK archive does not exist: ${PULP_VELLUM_SDK_ARCHIVE}")
    endif()
    file(SHA256 "${PULP_VELLUM_SDK_ARCHIVE}" _pulp_sdk_digest)
    if(NOT _pulp_sdk_digest STREQUAL PULP_VELLUM_sdk_digest)
        message(FATAL_ERROR "Pulp: SDK archive digest mismatch (lock ${PULP_VELLUM_sdk_digest}, file ${_pulp_sdk_digest})")
    endif()
endif()
# Same check for a caller that supplies the gpu-runtime archive outside the prefix path.
if(DEFINED PULP_VELLUM_GPU_RUNTIME_ARCHIVE AND NOT PULP_VELLUM_GPU_RUNTIME_ARCHIVE STREQUAL "")
    if(NOT EXISTS "${PULP_VELLUM_GPU_RUNTIME_ARCHIVE}")
        message(FATAL_ERROR "Pulp: gpu-runtime archive does not exist: ${PULP_VELLUM_GPU_RUNTIME_ARCHIVE}")
    endif()
    file(SHA256 "${PULP_VELLUM_GPU_RUNTIME_ARCHIVE}" _pulp_gpu_digest)
    if(NOT _pulp_gpu_digest STREQUAL PULP_VELLUM_gpu_runtime_digest)
        message(FATAL_ERROR "Pulp: gpu-runtime archive digest mismatch (lock ${PULP_VELLUM_gpu_runtime_digest}, file ${_pulp_gpu_digest})")
    endif()
endif()
set(PULP_GPU_RUNTIME_COMMIT "${PULP_VELLUM_sha}")
set(PULP_GPU_RUNTIME_VERSION "${PULP_VELLUM_tag}")
string(REGEX REPLACE "^v" "" PULP_GPU_RUNTIME_VERSION "${PULP_GPU_RUNTIME_VERSION}")
unset(_pulp_vellum_lock)
