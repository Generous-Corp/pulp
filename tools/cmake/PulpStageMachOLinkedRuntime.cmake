# Stage a registered runtime only when a final Mach-O image actually loads it.
# Static archives can expose optional providers which dead stripping removes
# from most consumers; inspecting the final load commands prevents those
# unrelated bundles from inheriting a large sidecar.

foreach(_required
        PULP_LINKED_IMAGE
        PULP_LINKED_RUNTIME
        PULP_LINKED_RUNTIME_NAME
        PULP_STAGING_DIR
        PULP_STAGING_OWNERSHIP_DIR
        PULP_STAGING_LABEL)
    if(NOT DEFINED ${_required} OR "${${_required}}" STREQUAL "")
        message(FATAL_ERROR "${_required} is required")
    endif()
endforeach()

if(NOT EXISTS "${PULP_LINKED_IMAGE}")
    message(FATAL_ERROR "linked image does not exist: ${PULP_LINKED_IMAGE}")
endif()
if(NOT EXISTS "${PULP_LINKED_RUNTIME}")
    message(FATAL_ERROR "linked runtime does not exist: ${PULP_LINKED_RUNTIME}")
endif()
if(DEFINED PULP_LINKED_ATTRIBUTION_DIR
   AND NOT PULP_LINKED_ATTRIBUTION_DIR STREQUAL "")
    if(NOT DEFINED PULP_LINKED_ATTRIBUTION_NAME
       OR NOT PULP_LINKED_ATTRIBUTION_NAME MATCHES
          "^[A-Za-z0-9][A-Za-z0-9._-]*$")
        message(FATAL_ERROR
            "PULP_LINKED_ATTRIBUTION_NAME must be a safe non-empty directory name")
    endif()
endif()

execute_process(
    COMMAND /usr/bin/otool -L "${PULP_LINKED_IMAGE}"
    RESULT_VARIABLE _otool_rc
    OUTPUT_VARIABLE _otool_out
    ERROR_VARIABLE _otool_err)
if(NOT _otool_rc EQUAL 0)
    message(FATAL_ERROR
        "otool failed for ${PULP_STAGING_LABEL} (${_otool_rc}): ${_otool_err}")
endif()

execute_process(
    COMMAND /usr/bin/otool -D "${PULP_LINKED_RUNTIME}"
    RESULT_VARIABLE _runtime_id_rc
    OUTPUT_VARIABLE _runtime_id_out
    ERROR_VARIABLE _runtime_id_err)
if(NOT _runtime_id_rc EQUAL 0)
    message(FATAL_ERROR
        "otool -D failed for ${PULP_LINKED_RUNTIME_NAME} "
        "(${_runtime_id_rc}): ${_runtime_id_err}")
endif()
string(REPLACE "\n" ";" _runtime_id_lines "${_runtime_id_out}")
list(LENGTH _runtime_id_lines _runtime_id_line_count)
if(_runtime_id_line_count LESS 2)
    message(FATAL_ERROR
        "${PULP_LINKED_RUNTIME_NAME} has no Mach-O install identity: "
        "${_runtime_id_out}")
endif()
list(GET _runtime_id_lines 1 _registered_runtime_id)
string(STRIP "${_registered_runtime_id}" _registered_runtime_id)
if(_registered_runtime_id STREQUAL "")
    message(FATAL_ERROR
        "${PULP_LINKED_RUNTIME_NAME} has an empty Mach-O install identity")
endif()

string(SHA256 _ownership_key
    "${PULP_STAGING_DIR}\n${_registered_runtime_id}\n${PULP_STAGING_LABEL}")
set(_ownership_marker "${PULP_STAGING_OWNERSHIP_DIR}/${_ownership_key}.owner")

set(_runtime_ref_found FALSE)
string(REPLACE "\n" ";" _otool_lines "${_otool_out}")
foreach(_otool_line IN LISTS _otool_lines)
    string(STRIP "${_otool_line}" _otool_line)
    # The install name may be an absolute path containing spaces. Match the
    # stable otool suffix from the right so the complete name remains intact.
    if(_otool_line MATCHES "^(.+) \\(compatibility version .+$")
        set(_loaded_runtime_ref "${CMAKE_MATCH_1}")
        if(_loaded_runtime_ref STREQUAL _registered_runtime_id)
            set(_runtime_ref_found TRUE)
            break()
        endif()
    endif()
endforeach()
if(NOT _runtime_ref_found)
    # Only a target that previously staged this runtime may clean it up. Before
    # doing so, inspect every other regular file in the shared output directory:
    # another executable/module may still load the exact registered LC_ID.
    set(_owned_by_target FALSE)
    if(EXISTS "${_ownership_marker}")
        set(_owned_by_target TRUE)
        file(STRINGS "${_ownership_marker}" _ownership_lines)
        foreach(_ownership_line IN LISTS _ownership_lines)
            if(_ownership_line MATCHES
               "^runtime_sha256=([0-9a-fA-F]+)$")
                string(LENGTH "${CMAKE_MATCH_1}" _owned_digest_length)
                if(_owned_digest_length EQUAL 64)
                    set(_owned_runtime_sha256 "${CMAKE_MATCH_1}")
                endif()
            elseif(_ownership_line MATCHES
                   "^license_sha256=([0-9a-fA-F]+)$")
                string(LENGTH "${CMAKE_MATCH_1}" _owned_digest_length)
                if(_owned_digest_length EQUAL 64)
                    set(_owned_license_sha256 "${CMAKE_MATCH_1}")
                endif()
            elseif(_ownership_line MATCHES
                   "^notice_sha256=([0-9a-fA-F]+)$")
                string(LENGTH "${CMAKE_MATCH_1}" _owned_digest_length)
                if(_owned_digest_length EQUAL 64)
                    set(_owned_notice_sha256 "${CMAKE_MATCH_1}")
                endif()
            elseif(_ownership_line MATCHES
                   "^dependencies_sha256=([0-9a-fA-F]+)$")
                string(LENGTH "${CMAKE_MATCH_1}" _owned_digest_length)
                if(_owned_digest_length EQUAL 64)
                    set(_owned_dependencies_sha256 "${CMAKE_MATCH_1}")
                endif()
            endif()
        endforeach()
    endif()
    set(_other_image_needs_runtime FALSE)
    if(_owned_by_target)
        file(GLOB _staging_entries LIST_DIRECTORIES FALSE
            "${PULP_STAGING_DIR}/*")
        foreach(_staging_entry IN LISTS _staging_entries)
            if(_staging_entry STREQUAL PULP_LINKED_IMAGE
               OR _staging_entry STREQUAL
                  "${PULP_STAGING_DIR}/${PULP_LINKED_RUNTIME_NAME}")
                continue()
            endif()
            execute_process(
                COMMAND /usr/bin/otool -L "${_staging_entry}"
                RESULT_VARIABLE _peer_otool_rc
                OUTPUT_VARIABLE _peer_otool_out
                ERROR_QUIET)
            if(NOT _peer_otool_rc EQUAL 0)
                continue()
            endif()
            string(REPLACE "\n" ";" _peer_otool_lines "${_peer_otool_out}")
            foreach(_peer_otool_line IN LISTS _peer_otool_lines)
                string(STRIP "${_peer_otool_line}" _peer_otool_line)
                if(_peer_otool_line MATCHES
                   "^(.+) \\(compatibility version .+$"
                   AND CMAKE_MATCH_1 STREQUAL _registered_runtime_id)
                    set(_other_image_needs_runtime TRUE)
                    break()
                endif()
            endforeach()
            if(_other_image_needs_runtime)
                break()
            endif()
        endforeach()
    endif()
    if(_owned_by_target AND _other_image_needs_runtime)
        # The peer's own receipt now owns the shared payload.
        file(REMOVE "${_ownership_marker}")
    elseif(_owned_by_target)
        set(_cleanup_complete TRUE)
        set(_staged_runtime
            "${PULP_STAGING_DIR}/${PULP_LINKED_RUNTIME_NAME}")
        if(EXISTS "${_staged_runtime}")
            if(DEFINED _owned_runtime_sha256)
                file(SHA256 "${_staged_runtime}" _runtime_staged_sha256)
                if(_owned_runtime_sha256 STREQUAL _runtime_staged_sha256)
                    file(REMOVE "${_staged_runtime}")
                else()
                    set(_cleanup_complete FALSE)
                endif()
            else()
                set(_cleanup_complete FALSE)
            endif()
        endif()
        if(DEFINED PULP_LINKED_ATTRIBUTION_DIR
           AND NOT PULP_LINKED_ATTRIBUTION_DIR STREQUAL "")
            set(_attribution_destination
                "${PULP_STAGING_DIR}/PulpThirdParty/${PULP_LINKED_ATTRIBUTION_NAME}")
            foreach(_attribution_file LICENSE.md NOTICE.md DEPENDENCIES.md)
                set(_attribution_source
                    "${PULP_LINKED_ATTRIBUTION_DIR}/${_attribution_file}")
                set(_staged_attribution
                    "${_attribution_destination}/${_attribution_file}")
                if(_attribution_file STREQUAL "LICENSE.md")
                    set(_owned_attribution_sha256
                        "${_owned_license_sha256}")
                elseif(_attribution_file STREQUAL "NOTICE.md")
                    set(_owned_attribution_sha256
                        "${_owned_notice_sha256}")
                else()
                    set(_owned_attribution_sha256
                        "${_owned_dependencies_sha256}")
                endif()
                if(NOT _owned_attribution_sha256 STREQUAL ""
                   AND EXISTS "${_staged_attribution}")
                    file(SHA256 "${_staged_attribution}"
                        _staged_attribution_sha256)
                    if(_owned_attribution_sha256 STREQUAL
                       _staged_attribution_sha256)
                        file(REMOVE "${_staged_attribution}")
                    else()
                        set(_cleanup_complete FALSE)
                    endif()
                elseif(EXISTS "${_staged_attribution}")
                    set(_cleanup_complete FALSE)
                endif()
            endforeach()
        endif()
        if(_cleanup_complete)
            file(REMOVE "${_ownership_marker}")
        else()
            message(WARNING
                "pulp-runtime-staging: ${PULP_STAGING_LABEL}: retained "
                "ownership receipt because staged bytes no longer match it")
        endif()
    endif()
    message(STATUS
        "pulp-runtime-staging: ${PULP_STAGING_LABEL}: skipped unlinked "
        "${PULP_LINKED_RUNTIME_NAME}")
    return()
endif()

execute_process(
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PULP_LINKED_RUNTIME}" "${PULP_STAGING_DIR}"
    RESULT_VARIABLE _copy_rc
    ERROR_VARIABLE _copy_err)
if(NOT _copy_rc EQUAL 0)
    message(FATAL_ERROR
        "failed to stage ${PULP_LINKED_RUNTIME_NAME} for "
        "${PULP_STAGING_LABEL}: ${_copy_err}")
endif()
if(NOT EXISTS "${PULP_STAGING_DIR}/${PULP_LINKED_RUNTIME_NAME}")
    message(FATAL_ERROR
        "${PULP_STAGING_LABEL}: copy completed without "
        "${PULP_LINKED_RUNTIME_NAME}")
endif()
file(SHA256 "${PULP_STAGING_DIR}/${PULP_LINKED_RUNTIME_NAME}"
    _receipt_runtime_sha256)

if(DEFINED PULP_LINKED_ATTRIBUTION_DIR
   AND NOT PULP_LINKED_ATTRIBUTION_DIR STREQUAL "")
    set(_attribution_destination
        "${PULP_STAGING_DIR}/PulpThirdParty/${PULP_LINKED_ATTRIBUTION_NAME}")
    file(MAKE_DIRECTORY "${_attribution_destination}")
    foreach(_attribution_file LICENSE.md NOTICE.md DEPENDENCIES.md)
        set(_attribution_source
            "${PULP_LINKED_ATTRIBUTION_DIR}/${_attribution_file}")
        if(NOT EXISTS "${_attribution_source}")
            message(FATAL_ERROR
                "${PULP_STAGING_LABEL}: linked ${PULP_LINKED_RUNTIME_NAME} "
                "without required attribution ${_attribution_source}")
        endif()
        file(COPY_FILE "${_attribution_source}"
            "${_attribution_destination}/${_attribution_file}"
            ONLY_IF_DIFFERENT)
        file(SHA256 "${_attribution_source}" _source_attribution_sha256)
        file(SHA256 "${_attribution_destination}/${_attribution_file}"
            _staged_attribution_sha256)
        if(NOT _source_attribution_sha256 STREQUAL
               _staged_attribution_sha256)
            message(FATAL_ERROR
                "${PULP_STAGING_LABEL}: staged ${PULP_LINKED_ATTRIBUTION_NAME} "
                "${_attribution_file} "
                "does not match its SDK source")
        endif()
        if(_attribution_file STREQUAL "LICENSE.md")
            set(_receipt_license_sha256 "${_staged_attribution_sha256}")
        elseif(_attribution_file STREQUAL "NOTICE.md")
            set(_receipt_notice_sha256 "${_staged_attribution_sha256}")
        else()
            set(_receipt_dependencies_sha256 "${_staged_attribution_sha256}")
        endif()
    endforeach()
endif()
file(MAKE_DIRECTORY "${PULP_STAGING_OWNERSHIP_DIR}")
file(WRITE "${_ownership_marker}"
    "version=1\n"
    "image=${PULP_LINKED_IMAGE}\n"
    "runtime_id=${_registered_runtime_id}\n"
    "runtime_sha256=${_receipt_runtime_sha256}\n")
if(DEFINED _receipt_license_sha256)
    file(APPEND "${_ownership_marker}"
        "license_sha256=${_receipt_license_sha256}\n"
        "notice_sha256=${_receipt_notice_sha256}\n"
        "dependencies_sha256=${_receipt_dependencies_sha256}\n")
endif()
message(STATUS
    "pulp-runtime-staging: ${PULP_STAGING_LABEL}: verified "
    "${PULP_LINKED_RUNTIME_NAME}")
