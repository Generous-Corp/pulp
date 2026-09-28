# PulpRuntimePins.cmake — the runtime dependency pins an SDK build links.
#
# Produces `share/pulp/runtime-pins.json` (schema `pulp.runtime-pins.v1`) in
# the build tree and installs it into the SDK, so the exact render/runtime
# stack survives SDK installation and can be copied into every bundle a
# consumer builds (PulpBuildInfo.cmake). Every value is read from the pin's
# existing single source of truth; nothing here restates a pin:
#
#   skia.release / commit / builder_ref  VERSION.md of the linked Skia tree
#   skia.asset_sha256                    the fetcher's .skia-asset-sha256 stamp,
#                                        checked against tools/deps/manifest.json
#   dawn.commit                          dawn/dawn_version.h (kDawnVersion)
#                                        from the resolved Skia include dirs
#   webgpu.wgpu_native_version           PULP_WGPU_NATIVE_VERSION
#                                        (PulpDependencies.cmake)
#   webgpu.distribution_ref              tools/deps/shared-source-contract.txt
#   js_engine                            PULP_JS_ENGINE + resolved backends
#   min_os                               tools/deps/min_os.json floors
#   pulp.sdk_version / source_git_*      project version + git at build time
#
# A value that cannot be resolved (for example a headers-only Skia checkout
# with no dawn_version.h) is recorded as null rather than guessed.
#
# Source-build only: the installed SDK ships the generated file, not this
# module.

include_guard(GLOBAL)
include("${CMAKE_CURRENT_LIST_DIR}/PulpBuildInfo.cmake")

# Parse release, branch-tip commit and skia-builder ref from VERSION.md.
function(_pulp_runtime_pins_skia version_md out_release out_commit out_builder_ref)
    set(_release "")
    set(_commit "")
    set(_builder_ref "")
    if(EXISTS "${version_md}")
        file(READ "${version_md}" _text)
        if(_text MATCHES "\\*\\*Release:\\*\\* ([^\n]+)")
            string(STRIP "${CMAKE_MATCH_1}" _release)
        endif()
        if(_text MATCHES "Skia branch tip is[ \t\r\n]*`([0-9a-f]+)`")
            set(_commit "${CMAKE_MATCH_1}")
        endif()
        if(_text MATCHES "skia-builder ref:\\*\\* `([0-9a-f]+)`")
            set(_builder_ref "${CMAKE_MATCH_1}")
        endif()
    endif()
    set(${out_release} "${_release}" PARENT_SCOPE)
    set(${out_commit} "${_commit}" PARENT_SCOPE)
    set(${out_builder_ref} "${_builder_ref}" PARENT_SCOPE)
endfunction()

# Decode dawn::kDawnVersion (20 bytes of SHA-1) into a hex commit id.
function(_pulp_runtime_pins_dawn_commit header out_var)
    set(_commit "")
    if(EXISTS "${header}")
        file(READ "${header}" _text)
        if(_text MATCHES "kDawnVersion[^{]*{([^}]*)}")
            string(REGEX MATCHALL "0x[0-9a-fA-F][0-9a-fA-F]" _bytes "${CMAKE_MATCH_1}")
            list(LENGTH _bytes _count)
            if(_count EQUAL 20)
                foreach(_byte IN LISTS _bytes)
                    string(SUBSTRING "${_byte}" 2 2 _hex)
                    string(TOLOWER "${_hex}" _hex)
                    string(APPEND _commit "${_hex}")
                endforeach()
            endif()
        endif()
    endif()
    set(${out_var} "${_commit}" PARENT_SCOPE)
endfunction()

function(_pulp_runtime_pins_find_dawn_header out_var)
    set(_found "")
    set(_candidates ${SKIA_INCLUDE_DIRS})
    if(SKIA_DIR)
        list(APPEND _candidates "${SKIA_DIR}/build/include" "${SKIA_DIR}/include")
    endif()
    foreach(_dir IN LISTS _candidates)
        if(EXISTS "${_dir}/dawn/dawn_version.h")
            set(_found "${_dir}/dawn/dawn_version.h")
            break()
        endif()
    endforeach()
    set(${out_var} "${_found}" PARENT_SCOPE)
endfunction()

# Render the min_os.json floors as {"<platform>": {"unit": ..., "floor": ...}}.
function(_pulp_runtime_pins_min_os min_os_json out_var)
    set(_json_out "{}")
    if(EXISTS "${min_os_json}")
        file(READ "${min_os_json}" _json)
        string(JSON _count ERROR_VARIABLE _error LENGTH "${_json}" platforms)
        if(NOT _error AND _count GREATER 0)
            set(_json_out "{")
            math(EXPR _last "${_count} - 1")
            foreach(_index RANGE ${_last})
                string(JSON _platform MEMBER "${_json}" platforms ${_index})
                string(JSON _unit ERROR_VARIABLE _unit_error
                    GET "${_json}" platforms "${_platform}" unit)
                string(JSON _floor ERROR_VARIABLE _floor_error
                    GET "${_json}" platforms "${_platform}" floor)
                if(_unit_error OR _unit STREQUAL "null")
                    set(_unit "")
                endif()
                if(_floor_error OR _floor STREQUAL "null")
                    set(_floor "")
                endif()
                _pulp_build_info_json_string(_j_platform "${_platform}")
                _pulp_build_info_json_string(_j_unit "${_unit}")
                _pulp_build_info_json_string(_j_floor "${_floor}")
                if(_index GREATER 0)
                    string(APPEND _json_out ",")
                endif()
                string(APPEND _json_out
                    "\n    ${_j_platform}: {\"unit\": ${_j_unit}, \"floor\": ${_j_floor}}")
            endforeach()
            string(APPEND _json_out "\n  }")
        endif()
    endif()
    set(${out_var} "${_json_out}" PARENT_SCOPE)
endfunction()

# Generate the pins record and the `pulp-runtime-pins` target that finalizes
# it at build time. Call from the root CMakeLists.txt after the dependency and
# JS-engine configuration has run.
function(pulp_configure_runtime_pins)
    set(_root "${PROJECT_SOURCE_DIR}")

    # Prefer the VERSION.md shipped with the Skia tree actually linked; an
    # explicit SKIA_DIR can point somewhere other than this checkout.
    set(_version_md "${_root}/external/skia-build/VERSION.md")
    if(SKIA_DIR AND EXISTS "${SKIA_DIR}/VERSION.md")
        set(_version_md "${SKIA_DIR}/VERSION.md")
    endif()
    _pulp_runtime_pins_skia("${_version_md}"
        _skia_release _skia_commit _skia_builder_ref)
    set(_dawn_commit "")
    set(_skia_linked FALSE)
    set(_skia_asset "")
    set(_j_skia_asset_in_manifest "null")
    if(PULP_HAS_SKIA)
        set(_skia_linked TRUE)
        _pulp_runtime_pins_find_dawn_header(_dawn_header)
        _pulp_runtime_pins_dawn_commit("${_dawn_header}" _dawn_commit)
        # The fetcher stamps each unpacked prebuilt with its archive digest.
        # Without the stamp (a hand-provisioned SKIA_DIR) the release above is
        # only what VERSION.md claims, and the record says so.
        if(SKIA_DIR AND EXISTS "${SKIA_DIR}/.skia-asset-sha256")
            file(READ "${SKIA_DIR}/.skia-asset-sha256" _skia_asset)
            string(STRIP "${_skia_asset}" _skia_asset)
            if(NOT _skia_asset MATCHES "^[0-9a-f]+$")
                set(_skia_asset "")
            endif()
        endif()
        if(_skia_asset)
            file(READ "${_root}/tools/deps/manifest.json" _manifest_text)
            string(FIND "${_manifest_text}" "\"${_skia_asset}\"" _asset_index)
            if(_asset_index GREATER -1)
                set(_j_skia_asset_in_manifest "true")
            else()
                set(_j_skia_asset_in_manifest "false")
            endif()
        endif()
    endif()

    set(_webgpu_backend "none")
    set(_wgpu_version "")
    set(_webgpu_ref "")
    if(PULP_HAS_WEBGPU)
        string(TOLOWER "${WEBGPU_BACKEND}" _backend_lower)
        if(_backend_lower STREQUAL "wgpu" OR _backend_lower STREQUAL "")
            set(_webgpu_backend "wgpu-native")
            set(_wgpu_version "${PULP_WGPU_NATIVE_VERSION}")
        else()
            set(_webgpu_backend "${_backend_lower}")
        endif()
        # The pinned-source contract setup.sh and PulpDependencies.cmake are
        # both checked against (test_setup_source_cache.sh).
        if(PULP_CHECKOUT_DEPENDENCY_CONTRACT MATCHES "(^|;)webgpu=([0-9a-f]+)")
            set(_webgpu_ref "${CMAKE_MATCH_2}")
        endif()
    endif()

    set(_js_default "none")
    set(_v8_version "")
    if(PULP_ENABLE_JS OR NOT DEFINED PULP_ENABLE_JS)
        set(_js_default "quickjs")
        if(PULP_JS_ENGINE STREQUAL "jsc" AND PULP_HAS_JSC_ACTUAL)
            set(_js_default "jsc")
        elseif(PULP_JS_ENGINE STREQUAL "v8" AND PULP_V8_PROVIDER_KIND)
            set(_js_default "v8")
            set(_v8_version "${PULP_V8_EXPECTED_RUNTIME_VERSION}")
        endif()
    endif()

    _pulp_runtime_pins_min_os("${_root}/tools/deps/min_os.json" _j_min_os)

    _pulp_build_info_json_string(_j_skia_release "${_skia_release}")
    _pulp_build_info_json_string(_j_skia_commit "${_skia_commit}")
    _pulp_build_info_json_string(_j_skia_builder_ref "${_skia_builder_ref}")
    _pulp_build_info_json_string(_j_dawn_commit "${_dawn_commit}")
    _pulp_build_info_json_string(_j_skia_asset "${_skia_asset}")
    _pulp_build_info_json_string(_j_webgpu_backend "${_webgpu_backend}")
    _pulp_build_info_json_string(_j_wgpu_version "${_wgpu_version}")
    _pulp_build_info_json_string(_j_webgpu_ref "${_webgpu_ref}")
    _pulp_build_info_json_string(_j_js_requested "${PULP_JS_ENGINE}")
    _pulp_build_info_json_string(_j_js_default "${_js_default}")
    _pulp_build_info_json_string(_j_v8_version "${_v8_version}")
    _pulp_build_info_json_string(_j_version "${PROJECT_VERSION}")
    if(_skia_linked)
        set(_j_skia_linked "true")
        set(_j_graphite_backend "\"dawn\"")
    else()
        set(_j_skia_linked "false")
        set(_j_graphite_backend "null")
    endif()

    set(_content "{\n")
    string(APPEND _content "  \"schema\": \"pulp.runtime-pins.v1\",\n")
    string(APPEND _content "  \"pulp\": {\n")
    string(APPEND _content "    \"sdk_version\": ${_j_version},\n")
    string(APPEND _content "    \"source_git_sha\": \"__PULP_GIT_SHA__\",\n")
    string(APPEND _content "    \"source_git_dirty\": __PULP_GIT_DIRTY__,\n")
    string(APPEND _content "    \"build_type\": \"$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,unspecified>\"\n")
    string(APPEND _content "  },\n")
    string(APPEND _content "  \"skia\": {\n")
    string(APPEND _content "    \"linked\": ${_j_skia_linked},\n")
    string(APPEND _content "    \"release\": ${_j_skia_release},\n")
    string(APPEND _content "    \"commit\": ${_j_skia_commit},\n")
    string(APPEND _content "    \"builder_ref\": ${_j_skia_builder_ref},\n")
    string(APPEND _content "    \"asset_sha256\": ${_j_skia_asset},\n")
    string(APPEND _content "    \"asset_in_manifest\": ${_j_skia_asset_in_manifest},\n")
    string(APPEND _content "    \"graphite_backend\": ${_j_graphite_backend}\n")
    string(APPEND _content "  },\n")
    string(APPEND _content "  \"dawn\": {\n")
    string(APPEND _content "    \"commit\": ${_j_dawn_commit}\n")
    string(APPEND _content "  },\n")
    string(APPEND _content "  \"webgpu\": {\n")
    string(APPEND _content "    \"backend\": ${_j_webgpu_backend},\n")
    string(APPEND _content "    \"wgpu_native_version\": ${_j_wgpu_version},\n")
    string(APPEND _content "    \"distribution_ref\": ${_j_webgpu_ref}\n")
    string(APPEND _content "  },\n")
    string(APPEND _content "  \"js_engine\": {\n")
    string(APPEND _content "    \"requested\": ${_j_js_requested},\n")
    string(APPEND _content "    \"default\": ${_j_js_default},\n")
    string(APPEND _content "    \"v8_runtime_version\": ${_j_v8_version}\n")
    string(APPEND _content "  },\n")
    string(APPEND _content "  \"min_os\": ${_j_min_os}\n")
    string(APPEND _content "}\n")

    set(_template "${CMAKE_BINARY_DIR}/pulp-runtime-pins/$<CONFIG>/runtime-pins.json.in")
    file(GENERATE OUTPUT "${_template}" CONTENT "${_content}")

    set(_output "${CMAKE_BINARY_DIR}/share/pulp/runtime-pins.json")
    find_package(Git QUIET)
    add_custom_target(pulp-runtime-pins ALL
        COMMAND "${CMAKE_COMMAND}"
            "-DPULP_BUILD_INFO_TEMPLATE=${_template}"
            "-DPULP_BUILD_INFO_OUTPUT=${_output}"
            "-DPULP_BUILD_INFO_GIT_DIR=${_root}"
            "-DPULP_BUILD_INFO_GIT=${GIT_EXECUTABLE}"
            -P "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpFinalizeBuildInfo.cmake"
        BYPRODUCTS "${_output}"
        COMMENT "Recording Pulp runtime pins"
        VERBATIM)
    install(FILES "${_output}" DESTINATION share/pulp)

    set(PULP_RUNTIME_PINS_FILE "${_output}" CACHE INTERNAL
        "Build-tree runtime pins record embedded into bundle build-info" FORCE)
    set(PULP_RUNTIME_PINS_TARGET pulp-runtime-pins CACHE INTERNAL
        "Target that produces PULP_RUNTIME_PINS_FILE" FORCE)
endfunction()
