# PulpBuildInfo.cmake — machine-readable build identity inside every bundle.
#
# Each plug-in / app bundle created by pulp_add_plugin() carries
# `pulp-build-info.json` (schema `pulp.build-info.v1`): the product identity
# (name, format, bundle id, version, source commit), how it was built (build
# type, architectures, minimum OS), the Pulp SDK it was built against, that
# SDK's runtime dependency pins, and the runtime libraries staged beside the
# binary. A support tool can then report what is installed from the bundle
# alone — Info.plist carries only a product version.
#
# Placement:
#   * Apple bundles  — <bundle>/Contents/Resources/pulp-build-info.json
#                      (flat iOS bundles: <bundle>/Resources/...). Written by a
#                      POST_BUILD step, so it exists before any signing step and
#                      is covered by the bundle's sealed resources.
#   * LV2            — <bundle>.lv2/pulp-build-info.json
#   * other modules  — <binary>.pulp-build-info.json beside the binary
#
# Product source commit: pass SOURCE_GIT_SHA / SOURCE_GIT_DIRTY to
# pulp_add_plugin(), or set PULP_PRODUCT_GIT_SHA / PULP_PRODUCT_GIT_DIRTY
# (e.g. `-DPULP_PRODUCT_GIT_SHA=$GITHUB_SHA` in CI). Otherwise the commit of
# the git checkout containing the calling CMakeLists.txt is read at build
# time, and "unknown" is recorded when there is none.
#
# Loaded by both the source build and find_package(Pulp) consumers (through
# PulpUtils.cmake), so the two produce the same record.

include_guard(GLOBAL)

# Quote `value` as a JSON string, or emit `null` for an empty value.
function(_pulp_build_info_json_string out_var value)
    if("${value}" STREQUAL "")
        set(${out_var} "null" PARENT_SCOPE)
        return()
    endif()
    string(REPLACE "\\" "\\\\" _escaped "${value}")
    string(REPLACE "\"" "\\\"" _escaped "${_escaped}")
    string(REPLACE "\n" "\\n" _escaped "${_escaped}")
    string(REPLACE "\t" "\\t" _escaped "${_escaped}")
    set(${out_var} "\"${_escaped}\"" PARENT_SCOPE)
endfunction()

function(_pulp_build_info_json_string_array out_var)
    set(_json "[")
    set(_first TRUE)
    foreach(_value IN LISTS ARGN)
        _pulp_build_info_json_string(_quoted "${_value}")
        if(NOT _first)
            string(APPEND _json ", ")
        endif()
        string(APPEND _json "${_quoted}")
        set(_first FALSE)
    endforeach()
    string(APPEND _json "]")
    set(${out_var} "${_json}" PARENT_SCOPE)
endfunction()

# The runtime-pins record this build links against: the SDK's installed copy
# for find_package(Pulp) consumers, the build tree's generated copy for a
# source build. Empty when neither exists (an SDK that predates the record).
function(_pulp_build_info_pins_file out_var)
    set(_pins "")
    if(DEFINED PULP_SDK_DIR AND EXISTS "${PULP_SDK_DIR}/share/pulp/runtime-pins.json")
        set(_pins "${PULP_SDK_DIR}/share/pulp/runtime-pins.json")
    elseif(PULP_RUNTIME_PINS_FILE)
        set(_pins "${PULP_RUNTIME_PINS_FILE}")
    endif()
    set(${out_var} "${_pins}" PARENT_SCOPE)
endfunction()

# The pinned wgpu-native release, from the same source the pins record uses.
function(_pulp_build_info_wgpu_version out_var)
    set(_version "")
    if(PULP_WGPU_NATIVE_VERSION)
        set(_version "${PULP_WGPU_NATIVE_VERSION}")
    else()
        _pulp_build_info_pins_file(_pins)
        if(_pins AND EXISTS "${_pins}")
            file(READ "${_pins}" _pins_json)
            string(JSON _version ERROR_VARIABLE _error
                GET "${_pins_json}" webgpu wgpu_native_version)
            if(_error OR _version STREQUAL "null")
                set(_version "")
            endif()
        endif()
    endif()
    set(${out_var} "${_version}" PARENT_SCOPE)
endfunction()

# File name of the wgpu-native runtime staged beside every module, or empty.
# A source build records it in WEBGPU_RUNTIME_LIB; a find_package(Pulp)
# consumer only has the imported `webgpu` target PulpWebGpuImportedTarget.cmake
# recreated from the SDK's lib directory.
function(_pulp_build_info_wgpu_runtime_file out_var)
    set(_file "")
    if(WEBGPU_RUNTIME_LIB)
        get_filename_component(_file "${WEBGPU_RUNTIME_LIB}" NAME)
    elseif(TARGET webgpu)
        get_target_property(_type webgpu TYPE)
        get_target_property(_imported webgpu IMPORTED)
        if(_imported AND _type STREQUAL "SHARED_LIBRARY")
            foreach(_property IMPORTED_LOCATION_RELEASE IMPORTED_LOCATION)
                get_target_property(_location webgpu ${_property})
                if(_location AND NOT _location MATCHES "-NOTFOUND$")
                    get_filename_component(_file "${_location}" NAME)
                    break()
                endif()
            endforeach()
        endif()
    endif()
    set(${out_var} "${_file}" PARENT_SCOPE)
endfunction()

function(_pulp_build_info_min_os out_platform out_version)
    set(_platform "")
    set(_version "")
    if(ANDROID)
        set(_platform "android")
        if(ANDROID_PLATFORM_LEVEL)
            set(_version "${ANDROID_PLATFORM_LEVEL}")
        elseif(ANDROID_NATIVE_API_LEVEL)
            set(_version "${ANDROID_NATIVE_API_LEVEL}")
        endif()
    elseif(APPLE AND (IOS OR PULP_IOS))
        set(_platform "ios")
        set(_version "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    elseif(APPLE)
        set(_platform "macos")
        set(_version "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    elseif(WIN32)
        set(_platform "windows")
        if(PULP_MIN_OS_JSON AND EXISTS "${PULP_MIN_OS_JSON}")
            file(READ "${PULP_MIN_OS_JSON}" _json)
            string(JSON _version ERROR_VARIABLE _error
                GET "${_json}" platforms windows-x64 floor)
            if(_error OR _version STREQUAL "null")
                set(_version "")
            endif()
        endif()
    elseif(UNIX)
        set(_platform "linux-glibc")
        set(_version "${PULP_LINUX_GLIBC_FLOOR}")
    endif()
    set(${out_platform} "${_platform}" PARENT_SCOPE)
    set(${out_version} "${_version}" PARENT_SCOPE)
endfunction()

function(_pulp_build_info_archs out_var)
    if(APPLE AND NOT "${CMAKE_OSX_ARCHITECTURES}" STREQUAL "")
        set(_archs ${CMAKE_OSX_ARCHITECTURES})
    elseif(APPLE)
        set(_archs "${CMAKE_HOST_SYSTEM_PROCESSOR}")
    elseif(ANDROID AND CMAKE_ANDROID_ARCH_ABI)
        set(_archs "${CMAKE_ANDROID_ARCH_ABI}")
    elseif(WIN32 AND CMAKE_GENERATOR_PLATFORM)
        string(TOLOWER "${CMAKE_GENERATOR_PLATFORM}" _archs)
    else()
        set(_archs "${CMAKE_SYSTEM_PROCESSOR}")
    endif()
    set(${out_var} "${_archs}" PARENT_SCOPE)
endfunction()

# Attach pulp-build-info.json to one bundle-producing target.
#
#   target        the pulp_add_plugin() product name (e.g. PulpGain)
#   format_target the CMake target that produces the bundle (PulpGain_VST3)
#   format        the format label recorded in the file (VST3, AU, CLAP, ...)
#   name / bundle_id / version / manufacturer — the product identity
function(_pulp_attach_build_info target format_target format name bundle_id version manufacturer)
    if(NOT TARGET ${format_target})
        return()
    endif()
    get_target_property(_is_framework ${format_target} FRAMEWORK)
    if(_is_framework)
        # A framework is an implementation detail embedded in a bundle that
        # carries its own record; a Contents/Resources directory at a
        # framework root would also break its code signature.
        return()
    endif()

    # Product source commit: per-target argument, then project-wide variable,
    # then the git checkout at build time.
    set(_git_sha "${PULP_${target}_SOURCE_GIT_SHA}")
    if(_git_sha STREQUAL "" AND DEFINED PULP_PRODUCT_GIT_SHA)
        set(_git_sha "${PULP_PRODUCT_GIT_SHA}")
    endif()
    set(_git_dirty "${PULP_${target}_SOURCE_GIT_DIRTY}")
    if(_git_dirty STREQUAL "" AND DEFINED PULP_PRODUCT_GIT_DIRTY)
        set(_git_dirty "${PULP_PRODUCT_GIT_DIRTY}")
    endif()
    if(NOT _git_dirty STREQUAL "")
        if(_git_dirty)
            set(_git_dirty TRUE)
        else()
            set(_git_dirty FALSE)
        endif()
    endif()

    _pulp_build_info_json_string(_j_name "${name}")
    _pulp_build_info_json_string(_j_target "${target}")
    _pulp_build_info_json_string(_j_format "${format}")
    _pulp_build_info_json_string(_j_bundle_id "${bundle_id}")
    _pulp_build_info_json_string(_j_version "${version}")
    _pulp_build_info_json_string(_j_manufacturer "${manufacturer}")

    _pulp_build_info_archs(_archs)
    _pulp_build_info_json_string_array(_j_archs ${_archs})
    _pulp_build_info_min_os(_min_os_platform _min_os_version)
    _pulp_build_info_json_string(_j_min_os_platform "${_min_os_platform}")
    _pulp_build_info_json_string(_j_min_os_version "${_min_os_version}")
    _pulp_build_info_json_string(_j_compiler_id "${CMAKE_CXX_COMPILER_ID}")
    _pulp_build_info_json_string(_j_compiler_version "${CMAKE_CXX_COMPILER_VERSION}")

    # SDK identity. A source build has no installed provenance marker.
    if(DEFINED Pulp_VERSION AND NOT "${Pulp_VERSION}" STREQUAL "")
        set(_sdk_version "${Pulp_VERSION}")
    else()
        set(_sdk_version "${PULP_VERSION}")
    endif()
    if(DEFINED PULP_SDK_DIR)
        set(_sdk_kind "${PULP_SDK_PROVENANCE_KIND}")
        set(_sdk_sha "${PULP_SDK_SOURCE_GIT_SHA}")
    else()
        set(_sdk_kind "source-tree")
        set(_sdk_sha "")
    endif()
    _pulp_build_info_json_string(_j_sdk_version "${_sdk_version}")
    _pulp_build_info_json_string(_j_sdk_kind "${_sdk_kind}")
    _pulp_build_info_json_string(_j_sdk_sha "${_sdk_sha}")

    # Runtime libraries staged beside this bundle's own binary by
    # pulp_stage_runtime_dependencies(). A bundle that only embeds another
    # module (the AUv3 extension and host app carry the framework, which
    # holds the libraries) lists none rather than claiming files it lacks.
    set(_libs "")
    get_target_property(_staged ${format_target} PULP_RUNTIME_DEPENDENCIES_STAGED)
    set(_wgpu_file "")
    if(_staged)
        _pulp_build_info_wgpu_runtime_file(_wgpu_file)
    endif()
    if(_wgpu_file)
        _pulp_build_info_wgpu_version(_wgpu_version)
        _pulp_build_info_json_string(_j_file "${_wgpu_file}")
        _pulp_build_info_json_string(_j_ver "${_wgpu_version}")
        list(APPEND _libs
            "{\"file\": ${_j_file}, \"component\": \"wgpu-native\", \"version\": ${_j_ver}}")
    endif()
    if(_staged AND WIN32 AND SKIA_FOUND)
        list(APPEND _libs
            "{\"file\": \"icudtl.dat\", \"component\": \"skia-icu-data\", \"version\": null}")
    endif()
    if(_staged AND COMMAND _pulp_registered_runtime_dependency_targets)
        _pulp_registered_runtime_dependency_targets(_registered)
        foreach(_runtime_target IN LISTS _registered)
            list(APPEND _libs
                "{\"file\": \"$<TARGET_FILE_NAME:${_runtime_target}>\", \"component\": \"${_runtime_target}\", \"version\": null}")
        endforeach()
    endif()
    set(_j_libs "[")
    set(_first TRUE)
    foreach(_lib IN LISTS _libs)
        if(NOT _first)
            string(APPEND _j_libs ",")
        endif()
        string(APPEND _j_libs "\n    ${_lib}")
        set(_first FALSE)
    endforeach()
    if(_first)
        string(APPEND _j_libs "]")
    else()
        string(APPEND _j_libs "\n  ]")
    endif()

    set(_content "{\n")
    string(APPEND _content "  \"schema\": \"pulp.build-info.v1\",\n")
    string(APPEND _content "  \"product\": {\n")
    string(APPEND _content "    \"name\": ${_j_name},\n")
    string(APPEND _content "    \"target\": ${_j_target},\n")
    string(APPEND _content "    \"format\": ${_j_format},\n")
    string(APPEND _content "    \"bundle_id\": ${_j_bundle_id},\n")
    string(APPEND _content "    \"version\": ${_j_version},\n")
    string(APPEND _content "    \"manufacturer\": ${_j_manufacturer},\n")
    string(APPEND _content "    \"source_git_sha\": \"__PULP_GIT_SHA__\",\n")
    string(APPEND _content "    \"source_git_dirty\": __PULP_GIT_DIRTY__\n")
    string(APPEND _content "  },\n")
    string(APPEND _content "  \"build\": {\n")
    string(APPEND _content "    \"type\": \"$<IF:$<BOOL:$<CONFIG>>,$<CONFIG>,unspecified>\",\n")
    string(APPEND _content "    \"archs\": ${_j_archs},\n")
    string(APPEND _content "    \"min_os\": {\"platform\": ${_j_min_os_platform}, \"version\": ${_j_min_os_version}},\n")
    string(APPEND _content "    \"compiler\": {\"id\": ${_j_compiler_id}, \"version\": ${_j_compiler_version}}\n")
    string(APPEND _content "  },\n")
    string(APPEND _content "  \"pulp_sdk\": {\n")
    string(APPEND _content "    \"version\": ${_j_sdk_version},\n")
    string(APPEND _content "    \"provenance_kind\": ${_j_sdk_kind},\n")
    string(APPEND _content "    \"source_git_sha\": ${_j_sdk_sha}\n")
    string(APPEND _content "  },\n")
    string(APPEND _content "  \"runtime_pins\": __PULP_EMBED__,\n")
    string(APPEND _content "  \"bundled_runtime_libraries\": ${_j_libs}\n")
    string(APPEND _content "}\n")

    set(_template_dir "${CMAKE_CURRENT_BINARY_DIR}/pulp-build-info/${format_target}")
    set(_template "${_template_dir}/$<CONFIG>/pulp-build-info.json.in")
    file(GENERATE OUTPUT "${_template}" CONTENT "${_content}")

    get_target_property(_is_bundle ${format_target} BUNDLE)
    get_target_property(_is_app_bundle ${format_target} MACOSX_BUNDLE)
    if("${format_target}" MATCHES "_LV2$")
        set(_output "$<TARGET_FILE_DIR:${format_target}>/pulp-build-info.json")
    elseif(APPLE AND (_is_bundle OR _is_app_bundle))
        set(_output
            "$<TARGET_BUNDLE_CONTENT_DIR:${format_target}>/Resources/pulp-build-info.json")
    else()
        set(_output
            "$<TARGET_FILE_DIR:${format_target}>/$<TARGET_FILE_BASE_NAME:${format_target}>.pulp-build-info.json")
    endif()

    _pulp_build_info_pins_file(_pins)
    find_package(Git QUIET)
    set(_finalize "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/PulpFinalizeBuildInfo.cmake")
    set(_finalize_command
        "${CMAKE_COMMAND}"
            "-DPULP_BUILD_INFO_TEMPLATE=${_template}"
            "-DPULP_BUILD_INFO_OUTPUT=${_output}"
            "-DPULP_BUILD_INFO_GIT_DIR=${CMAKE_CURRENT_SOURCE_DIR}"
            "-DPULP_BUILD_INFO_GIT_SHA=${_git_sha}"
            "-DPULP_BUILD_INFO_GIT_DIRTY=${_git_dirty}"
            "-DPULP_BUILD_INFO_EMBED=${_pins}"
            "-DPULP_BUILD_INFO_GIT=${GIT_EXECUTABLE}"
            -P "${_finalize}")

    # Two triggers, because neither alone keeps the record current:
    #  * POST_BUILD covers `--target <bundle>` builds — the record is present
    #    whenever the bundle was just linked.
    #  * the <format_target>_BuildInfo step (part of `all`) re-runs when the
    #    record's inputs change without a relink: the template (product
    #    version, build settings), the SDK pins, or a new commit — HEAD's
    #    reflog moves on every commit, checkout, and reset.
    # The finalize script only rewrites the file when its content changes.
    add_custom_command(TARGET ${format_target} POST_BUILD
        COMMAND ${_finalize_command}
        COMMENT "Writing pulp-build-info.json into ${format_target}"
        VERBATIM)

    set(_depends "${_template}" "${_finalize}" "$<TARGET_FILE:${format_target}>")
    if(_pins)
        list(APPEND _depends "${_pins}")
    endif()
    if(_git_sha STREQUAL "" AND GIT_EXECUTABLE)
        execute_process(
            COMMAND "${GIT_EXECUTABLE}" -C "${CMAKE_CURRENT_SOURCE_DIR}"
                rev-parse --absolute-git-dir
            RESULT_VARIABLE _git_dir_result
            OUTPUT_VARIABLE _git_dir
            OUTPUT_STRIP_TRAILING_WHITESPACE
            ERROR_QUIET)
        if(_git_dir_result EQUAL 0 AND EXISTS "${_git_dir}/logs/HEAD")
            list(APPEND _depends "${_git_dir}/logs/HEAD")
        endif()
    endif()
    set(_stamp "${_template_dir}/$<CONFIG>/pulp-build-info.stamp")
    add_custom_command(OUTPUT "${_stamp}"
        COMMAND ${_finalize_command}
        COMMAND "${CMAKE_COMMAND}" -E touch "${_stamp}"
        DEPENDS ${_depends}
        COMMENT "Refreshing pulp-build-info.json in ${format_target}"
        VERBATIM)
    add_custom_target(${format_target}_BuildInfo ALL DEPENDS "${_stamp}")
    add_dependencies(${format_target}_BuildInfo ${format_target})
    if(PULP_RUNTIME_PINS_TARGET AND TARGET ${PULP_RUNTIME_PINS_TARGET})
        add_dependencies(${format_target} ${PULP_RUNTIME_PINS_TARGET})
    endif()
    set_property(TARGET ${format_target} PROPERTY PULP_BUILD_INFO_FILE "${_output}")
    set_property(TARGET ${format_target} PROPERTY
        PULP_BUILD_INFO_TARGET ${format_target}_BuildInfo)
endfunction()
