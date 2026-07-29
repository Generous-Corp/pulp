# PulpAbletonLink.cmake — optional desktop Ableton Link SDK boundary
#
# Link is never bundled or fetched. Developers who have appropriate license
# rights enable this integration with an out-of-tree SDK checkout.

function(_pulp_ableton_link_escape_regex out_var input)
    string(REGEX REPLACE "([][+.*^$(){}|\\\\])" "\\\\\\1" _escaped "${input}")
    set(${out_var} "${_escaped}" PARENT_SCOPE)
endfunction()

function(pulp_configure_ableton_link_sdk)
    set(PULP_HAS_ABLETON_LINK FALSE PARENT_SCOPE)
    set(PULP_ABLETON_LINK_SDK_DIR_REAL "" PARENT_SCOPE)

    if(NOT PULP_ENABLE_ABLETON_LINK)
        if(NOT PULP_ABLETON_LINK_SDK_DIR AND
           DEFINED ENV{PULP_ABLETON_LINK_SDK_DIR} AND
           NOT "$ENV{PULP_ABLETON_LINK_SDK_DIR}" STREQUAL "")
            message(STATUS
                "Pulp: PULP_ABLETON_LINK_SDK_DIR is set in the environment, but "
                "Ableton Link support is disabled. Enable it with "
                "-DPULP_ENABLE_ABLETON_LINK=ON")
        endif()
        message(STATUS "Pulp: Ableton Link support disabled")
        return()
    endif()

    if(PULP_IOS OR ANDROID OR CMAKE_SYSTEM_NAME STREQUAL "WASI" OR
       CMAKE_SYSTEM_NAME STREQUAL "Emscripten" OR EMSCRIPTEN)
        message(FATAL_ERROR
            "Pulp: PULP_ENABLE_ABLETON_LINK is the desktop Ableton/link integration. "
            "It is not LinkKit and cannot be enabled for iOS, Android, or WebAssembly.")
    endif()

    if(NOT APPLE AND NOT WIN32 AND
       NOT CMAKE_SYSTEM_NAME MATCHES "Linux|kFreeBSD|GNU")
        message(FATAL_ERROR
            "Pulp: optional desktop Ableton Link support is available only on "
            "macOS, Windows, and Linux desktop targets")
    endif()

    set(_pulp_ableton_link_sdk_dir "${PULP_ABLETON_LINK_SDK_DIR}")
    if(NOT _pulp_ableton_link_sdk_dir AND
       DEFINED ENV{PULP_ABLETON_LINK_SDK_DIR} AND
       NOT "$ENV{PULP_ABLETON_LINK_SDK_DIR}" STREQUAL "")
        set(_pulp_ableton_link_sdk_dir "$ENV{PULP_ABLETON_LINK_SDK_DIR}")
    endif()

    if(NOT _pulp_ableton_link_sdk_dir)
        message(FATAL_ERROR
            "Pulp: PULP_ENABLE_ABLETON_LINK=ON requires "
            "PULP_ABLETON_LINK_SDK_DIR to point to a developer-supplied Ableton/link "
            "checkout kept outside the Pulp source tree")
    endif()
    if(NOT IS_DIRECTORY "${_pulp_ableton_link_sdk_dir}")
        message(FATAL_ERROR
            "Pulp: PULP_ABLETON_LINK_SDK_DIR does not exist or is not a directory: "
            "${_pulp_ableton_link_sdk_dir}")
    endif()

    file(REAL_PATH "${PULP_ROOT_DIR}" _pulp_repo_root_real)
    file(REAL_PATH "${_pulp_ableton_link_sdk_dir}" _pulp_ableton_link_sdk_real
        BASE_DIRECTORY "${PULP_ROOT_DIR}")
    _pulp_ableton_link_escape_regex(_pulp_repo_root_regex "${_pulp_repo_root_real}")
    if(_pulp_ableton_link_sdk_real STREQUAL _pulp_repo_root_real OR
       _pulp_ableton_link_sdk_real MATCHES "^${_pulp_repo_root_regex}(/|$)")
        message(FATAL_ERROR
            "Pulp: PULP_ABLETON_LINK_SDK_DIR must point outside the Pulp source tree. "
            "Do not clone or unpack Ableton/link into this repository.\n"
            "  repo: ${_pulp_repo_root_real}\n"
            "  sdk:  ${_pulp_ableton_link_sdk_real}")
    endif()

    set(_pulp_ableton_link_required_paths
        "AbletonLinkConfig.cmake"
        "cmake_include/ConfigureAbletonLink.cmake"
        "include/ableton/Link.hpp"
        "modules/asio-standalone/asio/include/asio.hpp"
    )
    foreach(_pulp_required_path IN LISTS _pulp_ableton_link_required_paths)
        if(NOT EXISTS "${_pulp_ableton_link_sdk_real}/${_pulp_required_path}")
            message(FATAL_ERROR
                "Pulp: PULP_ABLETON_LINK_SDK_DIR does not look like a complete "
                "desktop Ableton/link checkout.\n"
                "Missing: ${_pulp_required_path}\n"
                "SDK path: ${_pulp_ableton_link_sdk_real}\n"
                "Initialize its submodules with: git submodule update --init --recursive")
        endif()
    endforeach()

    include("${_pulp_ableton_link_sdk_real}/AbletonLinkConfig.cmake")
    if(NOT TARGET Ableton::Link)
        message(FATAL_ERROR
            "Pulp: AbletonLinkConfig.cmake did not define the expected Ableton::Link target")
    endif()

    set(PULP_HAS_ABLETON_LINK TRUE PARENT_SCOPE)
    set(PULP_ABLETON_LINK_SDK_DIR_REAL "${_pulp_ableton_link_sdk_real}" PARENT_SCOPE)
    message(STATUS
        "Pulp: desktop Ableton Link SDK available at ${_pulp_ableton_link_sdk_real} "
        "(developer-supplied, not redistributed)")
endfunction()
