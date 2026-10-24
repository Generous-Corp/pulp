# Read release floors from tools/scripts/release_product_matrix.json so the
# CLI and its tests compile against the same values the release workflow
# publishes with. The matrix is the only place a floor is written down;
# consumers receive it as a compile definition. `checksum_manifest_floor` is
# the first release whose publish step guaranteed SHA256SUMS; the manifest is
# verified whenever present, and the floor decides only what a missing one
# means (refusal at or above it, a warning below).

function(pulp_release_matrix_floor out_var key)
    set(_matrix "${PROJECT_SOURCE_DIR}/tools/scripts/release_product_matrix.json")
    file(READ "${_matrix}" _matrix_json)
    string(JSON _value ERROR_VARIABLE _error GET "${_matrix_json}" "${key}")
    if(_error)
        message(FATAL_ERROR "release_product_matrix.json has no `${key}`: ${_error}")
    endif()
    if(NOT _value MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+$")
        message(FATAL_ERROR "release_product_matrix.json `${key}` is not a semver: ${_value}")
    endif()
    set(${out_var} "${_value}" PARENT_SCOPE)
endfunction()
