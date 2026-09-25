# Machine-local cache of a dependency's configure-check results.
#
# A dependency's CheckSymbolExists / CheckIncludeFile / CheckCSourceCompiles
# calls each run a try_compile, and a fresh build directory repeats every one
# of them even though the answers depend only on the toolchain, the SDK and the
# dependency's own CMake sources. SDL3 alone runs ~180 of them (~40 s of a
# fresh configure). This module records the results the first time a machine
# configures with a given toolchain and replays them on the next fresh build
# directory, so the checks are skipped rather than re-derived.
#
# Correctness rests on three rules:
#   - The key hashes every input that can change a result: the dependency's
#     CMake sources, the C/OBJC compilers and their versions, the SDK (path and
#     SDKSettings.json), deployment target, architectures, system name and
#     processor, the CMAKE_C_FLAGS / CMAKE_REQUIRED_* inputs, the CMake version,
#     and every SDL_* style option already in the cache. A different toolchain
#     gets a different file; nothing is ever reused across keys.
#   - Only check results are recorded: INTERNAL booleans the dependency created
#     under the help string a CMake Check* module (or try_compile) writes, never
#     CMAKE_* internals, paths, or any other cached value.
#   - Apple hosts only. There the headers and libraries a check can see come
#     from the SDK named in the key. On Linux, installing a -dev package changes
#     results without changing any key input, so the cache stays off.
#
# Replayed variables are ordinary cache entries: an existing build directory,
# or a -D on the command line, always wins because seeding skips any variable
# that is already defined. PULP_CONFIGURE_CHECK_CACHE=OFF disables it.

if(APPLE)
    set(_pulp_configure_check_cache_default ON)
else()
    set(_pulp_configure_check_cache_default OFF)
endif()
option(PULP_CONFIGURE_CHECK_CACHE
    "Replay dependency configure-check results recorded for this exact toolchain"
    ${_pulp_configure_check_cache_default})
unset(_pulp_configure_check_cache_default)

set(PULP_CONFIGURE_CHECK_CACHE_SCHEMA 1)

# Help strings CMake's CheckSymbolExists / CheckIncludeFile(s) /
# CheckFunctionExists / CheckLibraryExists / CheckPrototypeDefinition /
# CheckVariableExists / Check<Lang>SourceCompiles / Check<Lang>CompilerFlag /
# CheckStructHasMember and a bare cached try_compile give their result.
set(_PULP_CONFIGURE_CHECK_HELP_REGEX
    "^(Have (symbol|include|includes|function|library|prototype|variable) .+|Test [A-Za-z0-9_]+|Result of TRY_COMPILE)$")

function(pulp_configure_check_cache_dir out_var)
    if(DEFINED ENV{PULP_CONFIGURE_CHECK_CACHE_DIR}
       AND NOT "$ENV{PULP_CONFIGURE_CHECK_CACHE_DIR}" STREQUAL "")
        set(${out_var} "$ENV{PULP_CONFIGURE_CHECK_CACHE_DIR}" PARENT_SCOPE)
    elseif(DEFINED ENV{HOME} AND NOT "$ENV{HOME}" STREQUAL "")
        set(${out_var} "$ENV{HOME}/Library/Caches/Pulp/configure-checks" PARENT_SCOPE)
    else()
        set(${out_var} "" PARENT_SCOPE)
    endif()
endfunction()

# Build the key text for NAME whose CMake sources live in SOURCE_DIR. Split
# out so the key's sensitivity can be tested without a toolchain.
function(pulp_configure_check_cache_key_text out_var name source_dir)
    set(text "schema=${PULP_CONFIGURE_CHECK_CACHE_SCHEMA}\nname=${name}\n")
    string(APPEND text "cmake=${CMAKE_VERSION}\n")

    file(GLOB_RECURSE _cmake_sources LIST_DIRECTORIES false
        "${source_dir}/CMakeLists.txt" "${source_dir}/cmake/*.cmake")
    list(SORT _cmake_sources)
    foreach(_src IN LISTS _cmake_sources)
        file(SHA256 "${_src}" _hash)
        file(RELATIVE_PATH _rel "${source_dir}" "${_src}")
        string(APPEND text "src:${_rel}=${_hash}\n")
    endforeach()

    foreach(_var IN ITEMS
            CMAKE_C_COMPILER CMAKE_C_COMPILER_ID CMAKE_C_COMPILER_VERSION
            CMAKE_OBJC_COMPILER CMAKE_OBJC_COMPILER_VERSION
            CMAKE_SYSTEM_NAME CMAKE_SYSTEM_VERSION CMAKE_SYSTEM_PROCESSOR
            CMAKE_OSX_SYSROOT CMAKE_OSX_DEPLOYMENT_TARGET CMAKE_OSX_ARCHITECTURES
            CMAKE_C_FLAGS CMAKE_OBJC_FLAGS CMAKE_EXE_LINKER_FLAGS
            CMAKE_REQUIRED_FLAGS CMAKE_REQUIRED_DEFINITIONS CMAKE_REQUIRED_INCLUDES
            CMAKE_REQUIRED_LIBRARIES CMAKE_REQUIRED_LINK_OPTIONS
            CMAKE_TRY_COMPILE_TARGET_TYPE CMAKE_TRY_COMPILE_CONFIGURATION
            CMAKE_POSITION_INDEPENDENT_CODE CMAKE_C_STANDARD)
        string(APPEND text "${_var}=${${_var}}\n")
    endforeach()

    # The SDK a check compiles against, identified by its own version record
    # (an Xcode update can keep the path and change the contents).
    set(_sdk "${CMAKE_OSX_SYSROOT}")
    if(_sdk AND NOT IS_ABSOLUTE "${_sdk}")
        execute_process(COMMAND xcrun --sdk "${_sdk}" --show-sdk-path
            OUTPUT_VARIABLE _sdk OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    endif()
    if(_sdk AND EXISTS "${_sdk}/SDKSettings.json")
        file(SHA256 "${_sdk}/SDKSettings.json" _sdk_hash)
        string(APPEND text "sdk=${_sdk}\nsdk_settings=${_sdk_hash}\n")
    else()
        string(APPEND text "sdk=${_sdk}\nsdk_settings=\n")
    endif()

    # Options the dependency reads (SDL_*), as far as they exist before it runs.
    get_cmake_property(_cache_vars CACHE_VARIABLES)
    list(SORT _cache_vars)
    string(TOUPPER "${name}" _upper)
    string(REGEX REPLACE "[0-9]+$" "" _prefix "${_upper}")
    foreach(_var IN LISTS _cache_vars)
        if(_var MATCHES "^${_prefix}_")
            string(APPEND text "opt:${_var}=${${_var}}\n")
        endif()
    endforeach()

    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

# Call immediately before FetchContent_MakeAvailable(NAME).
function(pulp_configure_check_cache_begin name source_dir)
    set(PULP_CONFIGURE_CHECK_CACHE_ACTIVE_${name} FALSE PARENT_SCOPE)
    if(NOT PULP_CONFIGURE_CHECK_CACHE OR NOT APPLE)
        return()
    endif()
    pulp_configure_check_cache_dir(_dir)
    if(_dir STREQUAL "" OR NOT EXISTS "${source_dir}/CMakeLists.txt")
        return()
    endif()

    pulp_configure_check_cache_key_text(_key_text "${name}" "${source_dir}")
    string(SHA256 _key "${_key_text}")
    string(SUBSTRING "${_key}" 0 24 _key)
    set(_file "${_dir}/${name}-${_key}.cmake")

    get_cmake_property(_before CACHE_VARIABLES)
    set(PULP_CONFIGURE_CHECK_CACHE_BEFORE_${name} "${_before}" PARENT_SCOPE)
    set(PULP_CONFIGURE_CHECK_CACHE_FILE_${name} "${_file}" PARENT_SCOPE)
    set(PULP_CONFIGURE_CHECK_CACHE_ACTIVE_${name} TRUE PARENT_SCOPE)

    if(EXISTS "${_file}")
        # The file holds `pulp_configure_check_cache_seed(<var> <value> <help>)`
        # lines; seeding never overrides a variable the build already has.
        include("${_file}")
        set(PULP_CONFIGURE_CHECK_CACHE_SEEDED_${name} TRUE PARENT_SCOPE)
        message(STATUS "${name}: replayed configure checks from ${_file}")
    else()
        set(PULP_CONFIGURE_CHECK_CACHE_SEEDED_${name} FALSE PARENT_SCOPE)
    endif()
endfunction()

macro(pulp_configure_check_cache_seed var value help)
    if(NOT DEFINED CACHE{${var}})
        set(${var} "${value}" CACHE INTERNAL "${help}")
    endif()
endmacro()

# Call immediately after FetchContent_MakeAvailable(NAME). Records the result
# of every check NAME ran, when this configure ran them itself.
function(pulp_configure_check_cache_end name)
    if(NOT PULP_CONFIGURE_CHECK_CACHE_ACTIVE_${name}
       OR PULP_CONFIGURE_CHECK_CACHE_SEEDED_${name})
        return()
    endif()
    get_cmake_property(_after CACHE_VARIABLES)
    set(_before "${PULP_CONFIGURE_CHECK_CACHE_BEFORE_${name}}")
    set(_body "# Generated by PulpConfigureCheckCache.cmake. Safe to delete.\n")
    set(_count 0)
    list(SORT _after)
    foreach(_var IN LISTS _after)
        if(_var IN_LIST _before OR _var MATCHES "^CMAKE_")
            continue()
        endif()
        get_property(_type CACHE "${_var}" PROPERTY TYPE)
        get_property(_help CACHE "${_var}" PROPERTY HELPSTRING)
        set(_value "${${_var}}")
        # Only what CMake's Check* modules and try_compile store: an INTERNAL
        # boolean under the help string that module writes.
        if(NOT _type STREQUAL "INTERNAL"
           OR NOT _help MATCHES "${_PULP_CONFIGURE_CHECK_HELP_REGEX}"
           OR NOT _value MATCHES "^(1|0|TRUE|FALSE|ON|OFF)?$")
            continue()
        endif()
        foreach(_field IN ITEMS _value _help)
            string(REPLACE "\\" "\\\\" ${_field} "${${_field}}")
            string(REPLACE "\"" "\\\"" ${_field} "${${_field}}")
            string(REPLACE "$" "\\$" ${_field} "${${_field}}")
            string(REPLACE ";" "\\;" ${_field} "${${_field}}")
            string(REPLACE "\n" "\\n" ${_field} "${${_field}}")
        endforeach()
        string(APPEND _body "pulp_configure_check_cache_seed(${_var} \"${_value}\" \"${_help}\")\n")
        math(EXPR _count "${_count} + 1")
    endforeach()

    if(_count EQUAL 0)
        return()
    endif()
    set(_file "${PULP_CONFIGURE_CHECK_CACHE_FILE_${name}}")
    get_filename_component(_dir "${_file}" DIRECTORY)
    string(RANDOM LENGTH 8 _nonce)
    file(MAKE_DIRECTORY "${_dir}")
    file(WRITE "${_file}.${_nonce}.tmp" "${_body}")
    file(RENAME "${_file}.${_nonce}.tmp" "${_file}" RESULT _rename_result)
    if(NOT _rename_result EQUAL 0)
        file(REMOVE "${_file}.${_nonce}.tmp")
        return()
    endif()
    message(STATUS "${name}: recorded ${_count} configure-check results to ${_file}")
endfunction()
