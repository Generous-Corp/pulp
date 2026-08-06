function(pulp_default_fetchcontent_cache_root out_var)
    if(DEFINED ENV{PULP_SHARED_FETCHCONTENT_SOURCE_DIR} AND NOT "$ENV{PULP_SHARED_FETCHCONTENT_SOURCE_DIR}" STREQUAL "")
        set(${out_var} "$ENV{PULP_SHARED_FETCHCONTENT_SOURCE_DIR}" PARENT_SCOPE)
        return()
    endif()

    if(NOT "${PULP_SHARED_FETCHCONTENT_SOURCE_DIR}" STREQUAL "")
        set(${out_var} "${PULP_SHARED_FETCHCONTENT_SOURCE_DIR}" PARENT_SCOPE)
        return()
    endif()

    if(APPLE AND DEFINED ENV{HOME})
        set(${out_var} "$ENV{HOME}/Library/Caches/Pulp/fetchcontent-src" PARENT_SCOPE)
    elseif(WIN32)
        if(DEFINED ENV{LOCALAPPDATA} AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
            set(${out_var} "$ENV{LOCALAPPDATA}/Pulp/fetchcontent-src" PARENT_SCOPE)
        elseif(DEFINED ENV{USERPROFILE} AND NOT "$ENV{USERPROFILE}" STREQUAL "")
            set(${out_var} "$ENV{USERPROFILE}/AppData/Local/Pulp/fetchcontent-src" PARENT_SCOPE)
        else()
            set(${out_var} "" PARENT_SCOPE)
        endif()
    else()
        if(DEFINED ENV{XDG_CACHE_HOME} AND NOT "$ENV{XDG_CACHE_HOME}" STREQUAL "")
            set(${out_var} "$ENV{XDG_CACHE_HOME}/pulp/fetchcontent-src" PARENT_SCOPE)
        elseif(DEFINED ENV{HOME})
            set(${out_var} "$ENV{HOME}/.cache/pulp/fetchcontent-src" PARENT_SCOPE)
        else()
            set(${out_var} "" PARENT_SCOPE)
        endif()
    endif()
endfunction()

function(pulp_configure_fetchcontent_base_dir)
    if(NOT WIN32)
        return()
    endif()

    if(DEFINED FETCHCONTENT_BASE_DIR AND NOT "${FETCHCONTENT_BASE_DIR}" STREQUAL "")
        return()
    endif()

    if(DEFINED ENV{PULP_FETCHCONTENT_BASE_DIR} AND NOT "$ENV{PULP_FETCHCONTENT_BASE_DIR}" STREQUAL "")
        set(FETCHCONTENT_BASE_DIR "$ENV{PULP_FETCHCONTENT_BASE_DIR}" CACHE PATH
            "Base directory for FetchContent dependency subbuilds" FORCE)
        return()
    endif()

    if(DEFINED ENV{LOCALAPPDATA} AND NOT "$ENV{LOCALAPPDATA}" STREQUAL "")
        set(_pulp_fetchcontent_base "$ENV{LOCALAPPDATA}/Pulp/fc")
    elseif(DEFINED ENV{USERPROFILE} AND NOT "$ENV{USERPROFILE}" STREQUAL "")
        set(_pulp_fetchcontent_base "$ENV{USERPROFILE}/AppData/Local/Pulp/fc")
    else()
        set(_pulp_fetchcontent_base "${CMAKE_BINARY_DIR}/fc")
    endif()

    # MSBuild still trips over MAX_PATH in generated FetchContent subbuilds.
    # Keep transient dependency build trees out of deep Actions workspaces.
    set(FETCHCONTENT_BASE_DIR "${_pulp_fetchcontent_base}" CACHE PATH
        "Base directory for FetchContent dependency subbuilds" FORCE)
    unset(_pulp_fetchcontent_base)
endfunction()

pulp_configure_fetchcontent_base_dir()

function(pulp_sanitize_cache_suffix input out_var)
    string(REGEX REPLACE "[^A-Za-z0-9._-]+" "_" sanitized "${input}")
    string(REGEX REPLACE "_+" "_" sanitized "${sanitized}")
    string(REGEX REPLACE "^_+|_+$" "" sanitized "${sanitized}")
    set(${out_var} "${sanitized}" PARENT_SCOPE)
endfunction()

function(pulp_register_fetchcontent_source name)
    set(options)
    set(one_value_args DIR REF)
    cmake_parse_arguments(PFC "${options}" "${one_value_args}" "" ${ARGN})

    string(TOUPPER "${name}" upper_name)
    string(TOLOWER "${name}" lower_name)
    set(fetchcontent_var "FETCHCONTENT_SOURCE_DIR_${upper_name}")
    set(shared_marker "PULP_FETCHCONTENT_SOURCE_IS_SHARED_${upper_name}")
    set(shared_path_marker "PULP_FETCHCONTENT_SHARED_SELECTED_PATH_${upper_name}")

    set(expected_shared_candidate "")
    if(PULP_USE_SHARED_FETCHCONTENT_SOURCES)
        pulp_default_fetchcontent_cache_root(cache_root)
        if(NOT "${cache_root}" STREQUAL "")
            if(PFC_REF)
                pulp_sanitize_cache_suffix("${PFC_REF}" cache_suffix)
            endif()
            if(PFC_DIR)
                set(cache_dir "${PFC_DIR}")
            else()
                set(cache_dir "${lower_name}")
            endif()
            if(DEFINED cache_suffix AND NOT "${cache_suffix}" STREQUAL "")
                set(cache_dir "${cache_dir}-${cache_suffix}")
            endif()
            set(expected_shared_candidate "${cache_root}/${cache_dir}")
        endif()
    endif()

    if(DEFINED ${fetchcontent_var} AND NOT "${${fetchcontent_var}}" STREQUAL "")
        if(DEFINED ${shared_marker})
            set(source_is_shared "${${shared_marker}}")
            if(source_is_shared
               AND DEFINED ${shared_path_marker}
               AND NOT "${${shared_path_marker}}" STREQUAL "${${fetchcontent_var}}")
                # The cached automatic path was explicitly replaced by the
                # developer on this configure.
                set(source_is_shared FALSE)
            elseif(source_is_shared
                   AND NOT "${expected_shared_candidate}" STREQUAL ""
                   AND EXISTS "${expected_shared_candidate}"
                   AND NOT "${${fetchcontent_var}}" STREQUAL "${expected_shared_candidate}")
                # A pin or cache-root change moves automatic selections to the
                # new immutable cache without ever exposing the old cache to
                # Pulp's patch steps.
                set(${fetchcontent_var} "${expected_shared_candidate}" CACHE PATH
                    "Local source override for ${name}" FORCE)
            endif()
        else()
            # Migration from a build configured before the origin marker
            # existed. The prior implementation used this exact cache help
            # string for automatic selections; command-line and project-owned
            # overrides have different provenance even when they happen to
            # live beneath the cache root.
            get_property(existing_source_help CACHE ${fetchcontent_var} PROPERTY HELPSTRING)
            pulp_default_fetchcontent_cache_root(existing_cache_root)
            set(source_is_shared FALSE)
            if(existing_source_help STREQUAL "Local source override for ${name}"
               AND NOT "${existing_cache_root}" STREQUAL ""
               AND EXISTS "${existing_cache_root}")
                file(REAL_PATH "${existing_cache_root}" existing_cache_root_real)
                file(REAL_PATH "${${fetchcontent_var}}" existing_source_real)
                cmake_path(IS_PREFIX existing_cache_root_real "${existing_source_real}"
                    NORMALIZE source_is_shared)
            endif()
        endif()
        set(${shared_marker} ${source_is_shared} CACHE INTERNAL
            "Whether ${name} source was selected from Pulp's shared cache" FORCE)
        if(source_is_shared)
            set(${shared_path_marker} "${${fetchcontent_var}}" CACHE INTERNAL
                "Last automatic shared-cache path selected for ${name}" FORCE)
        else()
            unset(${shared_path_marker} CACHE)
        endif()
        if(EXISTS "${${fetchcontent_var}}")
            if(source_is_shared)
                message(STATUS "Pulp: using shared cache for ${name}: ${${fetchcontent_var}}")
            else()
                message(STATUS "Pulp: using explicit source override for ${name}: ${${fetchcontent_var}}")
            endif()
        else()
            message(WARNING "Pulp: explicit source override for ${name} does not exist: ${${fetchcontent_var}}")
        endif()
        return()
    endif()

    set(explicit_env "PULP_FETCHCONTENT_SOURCE_DIR_${upper_name}")
    set(candidate "")
    if(DEFINED ENV{${explicit_env}} AND NOT "$ENV{${explicit_env}}" STREQUAL "")
        set(candidate "$ENV{${explicit_env}}")
        set(source_label "env:${explicit_env}")
        set(source_is_shared FALSE)
    elseif(PULP_USE_SHARED_FETCHCONTENT_SOURCES)
        if(NOT "${expected_shared_candidate}" STREQUAL "")
            set(candidate "${expected_shared_candidate}")
            set(source_label "shared cache")
            set(source_is_shared TRUE)
        endif()
    endif()

    if("${candidate}" STREQUAL "")
        return()
    endif()

    if(EXISTS "${candidate}")
        set(${fetchcontent_var} "${candidate}" CACHE PATH "Local source override for ${name}" FORCE)
        set(${shared_marker} ${source_is_shared} CACHE INTERNAL
            "Whether ${name} source was selected from Pulp's shared cache" FORCE)
        if(source_is_shared)
            set(${shared_path_marker} "${candidate}" CACHE INTERNAL
                "Last automatic shared-cache path selected for ${name}" FORCE)
        else()
            unset(${shared_path_marker} CACHE)
        endif()
        message(STATUS "Pulp: using ${source_label} for ${name}: ${candidate}")
    elseif(DEFINED source_label AND source_label MATCHES "^env:")
        message(WARNING "Pulp: source override path for ${name} does not exist: ${candidate}")
    endif()
endfunction()

# Shared source caches are safe only while consumers treat them as immutable.
# Dependencies that Pulp patches during configure get a small build-local copy;
# explicit developer overrides remain untouched so dependency work can point at
# a writable checkout deliberately.
function(pulp_materialize_mutable_fetchcontent_source name)
    set(options)
    set(one_value_args PATCH_KEY)
    cmake_parse_arguments(PMFS "${options}" "${one_value_args}" "" ${ARGN})

    string(TOUPPER "${name}" upper_name)
    string(TOLOWER "${name}" lower_name)
    set(source_var "FETCHCONTENT_SOURCE_DIR_${upper_name}")
    set(shared_marker "PULP_FETCHCONTENT_SOURCE_IS_SHARED_${upper_name}")
    if(NOT DEFINED ${source_var} OR "${${source_var}}" STREQUAL "")
        return()
    endif()

    # Explicit developer overrides are writable inputs, even when their path
    # happens to sit below the configured cache root. Only sources selected by
    # pulp_register_fetchcontent_source are immutable shared-cache entries.
    if(NOT DEFINED ${shared_marker} OR NOT ${${shared_marker}})
        return()
    endif()

    pulp_default_fetchcontent_cache_root(cache_root)
    if("${cache_root}" STREQUAL "" OR NOT EXISTS "${cache_root}")
        return()
    endif()
    file(REAL_PATH "${cache_root}" cache_root_real)
    file(REAL_PATH "${${source_var}}" source_real)
    cmake_path(IS_PREFIX cache_root_real "${source_real}" NORMALIZE source_is_shared)
    if(NOT source_is_shared)
        return()
    endif()

    set(local_source "${CMAKE_BINARY_DIR}/_deps/pulp-mutable-${lower_name}-src")
    set(stamp "${local_source}/.pulp-source-key")
    set(expected_stamp "${source_real}\n${PMFS_PATCH_KEY}\n")
    set(current_stamp "")
    if(EXISTS "${stamp}")
        file(READ "${stamp}" current_stamp)
    endif()
    if(NOT current_stamp STREQUAL expected_stamp)
        file(REMOVE_RECURSE "${local_source}")
        file(MAKE_DIRECTORY "${local_source}")
        file(COPY "${source_real}/" DESTINATION "${local_source}" PATTERN ".git" EXCLUDE)
        file(WRITE "${stamp}" "${expected_stamp}")
    endif()

    set(${source_var} "${local_source}" PARENT_SCOPE)
    message(STATUS "Pulp: using build-local mutable copy for ${name}: ${local_source}")
endfunction()

function(pulp_register_wgpu_native_precompiled_source version)
    if(CMAKE_SYSTEM_NAME STREQUAL "Windows")
        set(url_os "windows")
        if(MSVC)
            set(url_compiler "msvc")
        else()
            set(url_compiler "gnu")
        endif()
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Linux")
        set(url_os "linux")
    elseif(CMAKE_SYSTEM_NAME STREQUAL "Darwin")
        set(url_os "macos")
    else()
        return()
    endif()

    string(TOLOWER "${CMAKE_SYSTEM_PROCESSOR}" arch)
    # On Apple, CMAKE_SYSTEM_PROCESSOR reports the HOST arch. Select the wgpu
    # slice from the TARGET arch (CMAKE_OSX_ARCHITECTURES) instead, so a cross
    # or Intel-thin build caches/fetches the matching slice rather than the
    # host's. A universal (arm64;x86_64) or unset value keeps the host arch as
    # the single-slice placeholder — the fat dylib is fabricated separately by
    # PulpWgpuUniversal.cmake, which overrides IMPORTED_LOCATION. This mirrors
    # the ARCH override PulpDependencies.cmake applies before fetching wgpu.
    if(CMAKE_SYSTEM_NAME STREQUAL "Darwin" AND CMAKE_OSX_ARCHITECTURES)
        set(_wgpu_has_arm64 FALSE)
        set(_wgpu_has_x86_64 FALSE)
        if(CMAKE_OSX_ARCHITECTURES MATCHES "arm64")
            set(_wgpu_has_arm64 TRUE)
        endif()
        if(CMAKE_OSX_ARCHITECTURES MATCHES "x86_64")
            set(_wgpu_has_x86_64 TRUE)
        endif()
        if(_wgpu_has_arm64 AND _wgpu_has_x86_64)
            # universal — keep the host arch placeholder (see comment above)
        elseif(_wgpu_has_x86_64)
            set(arch "x86_64")
        elseif(_wgpu_has_arm64)
            set(arch "arm64")
        endif()
    endif()
    if(arch STREQUAL "amd64")
        set(url_arch "x86_64")
    elseif(arch STREQUAL "x86_64")
        set(url_arch "x86_64")
    elseif(arch STREQUAL "arm64")
        set(url_arch "aarch64")
    elseif(arch STREQUAL "aarch64")
        set(url_arch "aarch64")
    elseif(arch STREQUAL "x86")
        set(url_arch "i686")
    elseif(arch STREQUAL "i686")
        set(url_arch "i686")
    else()
        return()
    endif()

    set(url_name "wgpu-${url_os}-${url_arch}")
    if(DEFINED url_compiler)
        set(url_name "${url_name}-${url_compiler}")
    endif()
    set(url_name "${url_name}-release")

    pulp_register_fetchcontent_source("${url_name}" DIR "${url_name}" REF "${version}")
endfunction()
