# PulpSparkle.cmake — embed Sparkle 2 in a macOS standalone app.
#
#   pulp_add_sparkle(MyPlugin_Standalone
#       FEED_URL      "https://github.com/me/app/releases/latest/download/appcast.xml"
#       PUBLIC_ED_KEY "<base64 Ed25519 public key>"
#       [AUTOMATIC_CHECKS ON|OFF]      # writes SUEnableAutomaticChecks; omit to
#                                      # let Sparkle ask the user on 2nd launch
#       [CHECK_INTERVAL <seconds>]     # SUScheduledCheckInterval
#       [KEEP_XPC_SERVICES]            # keep Installer/Downloader.xpc (sandboxed apps)
#       [VERSION 2.10.0 SHA256 <hash>] # override the pinned distribution
#       [DIST_DIR <dir>])              # use an already-extracted distribution
#
# Call it AFTER the app target exists (pulp_add_plugin(... FORMATS Standalone)
# or pulp_add_app()). Only ever pass the standalone .app target: Sparkle updates
# the app that contains it, and a plug-in bundle has no business carrying an
# updater (it runs inside someone else's process). The function refuses any
# target that is not a MACOSX_BUNDLE executable.
#
# What it does:
#   * downloads the pinned Sparkle release archive once per build tree,
#     verifying its SHA-256 (no floating "latest"),
#   * copies Sparkle.framework into <App>.app/Contents/Frameworks with ditto,
#     so the framework's Versions/ symlinks survive,
#   * drops XPCServices/ unless KEEP_XPC_SERVICES — Sparkle 2 needs them only
#     for sandboxed apps, and every extra nested bundle is one more thing to
#     sign and notarize — then re-signs the framework ad hoc so a development
#     build still verifies,
#   * links the framework (-needed_framework so the linker keeps the load
#     command even though the app references no Sparkle symbol directly) with
#     an @executable_path/../Frameworks rpath,
#   * writes SUFeedURL / SUPublicEDKey (and the optional keys) into the built
#     Info.plist.
#
# The SDK side (pulp-standalone) finds Sparkle at run time through the
# Objective-C runtime and adds "Check for Updates…" to the app menu; see
# core/format/include/pulp/format/detail/standalone_updater.hpp.
#
# Release signing is the installer recipe's job: build_combined_installer.sh
# signs every nested framework inside-out (Autoupdate, Updater.app, the
# framework) with the Developer ID identity and the hardened runtime before
# sealing the app.

set(PULP_SPARKLE_DEFAULT_VERSION "2.10.0")
set(PULP_SPARKLE_DEFAULT_SHA256
    "c2bf58aa8387266ac179357b1415d6f2635f044da8be41042af32425dae6da0c")

# Resolve (download + verify + extract) a Sparkle distribution and return the
# directory that contains Sparkle.framework and bin/.
function(pulp_resolve_sparkle_distribution out_dir)
    cmake_parse_arguments(ARG "" "VERSION;SHA256;DIST_DIR" "" ${ARGN})
    if(ARG_DIST_DIR)
        if(NOT EXISTS "${ARG_DIST_DIR}/Sparkle.framework")
            message(FATAL_ERROR
                "pulp_add_sparkle: DIST_DIR has no Sparkle.framework: ${ARG_DIST_DIR}")
        endif()
        set(${out_dir} "${ARG_DIST_DIR}" PARENT_SCOPE)
        return()
    endif()
    set(_version "${ARG_VERSION}")
    set(_sha256 "${ARG_SHA256}")
    if(NOT _version)
        set(_version "${PULP_SPARKLE_DEFAULT_VERSION}")
        set(_sha256 "${PULP_SPARKLE_DEFAULT_SHA256}")
    elseif(NOT _sha256)
        message(FATAL_ERROR
            "pulp_add_sparkle: VERSION ${_version} needs its SHA256; an unverified "
            "download would put an unauthenticated updater inside a signed app")
    endif()

    set(_root "${CMAKE_BINARY_DIR}/_deps/sparkle-${_version}")
    set(_archive "${_root}/Sparkle-${_version}.tar.xz")
    set(_stamp "${_root}/.extracted-${_sha256}")
    if(NOT EXISTS "${_stamp}")
        file(MAKE_DIRECTORY "${_root}")
        if(NOT EXISTS "${_archive}")
            message(STATUS "Pulp: downloading Sparkle ${_version}")
            file(DOWNLOAD
                "https://github.com/sparkle-project/Sparkle/releases/download/${_version}/Sparkle-${_version}.tar.xz"
                "${_archive}"
                EXPECTED_HASH SHA256=${_sha256}
                TLS_VERIFY ON
                STATUS _status)
            list(GET _status 0 _code)
            if(NOT _code EQUAL 0)
                file(REMOVE "${_archive}")
                message(FATAL_ERROR "pulp_add_sparkle: download failed: ${_status}")
            endif()
        else()
            file(SHA256 "${_archive}" _have)
            if(NOT _have STREQUAL _sha256)
                file(REMOVE "${_archive}")
                message(FATAL_ERROR
                    "pulp_add_sparkle: cached ${_archive} does not match the pinned "
                    "SHA-256; removed it, re-run configure")
            endif()
        endif()
        file(REMOVE_RECURSE "${_root}/dist")
        file(MAKE_DIRECTORY "${_root}/dist")
        # CMake's own tar preserves the framework's Versions/ symlinks.
        execute_process(
            COMMAND "${CMAKE_COMMAND}" -E tar xf "${_archive}"
            WORKING_DIRECTORY "${_root}/dist"
            RESULT_VARIABLE _rc)
        if(NOT _rc EQUAL 0 OR NOT EXISTS "${_root}/dist/Sparkle.framework")
            message(FATAL_ERROR "pulp_add_sparkle: could not extract ${_archive}")
        endif()
        file(WRITE "${_stamp}" "${_version}\n")
    endif()
    set(${out_dir} "${_root}/dist" PARENT_SCOPE)
endfunction()

function(pulp_add_sparkle target)
    cmake_parse_arguments(ARG "KEEP_XPC_SERVICES"
        "FEED_URL;PUBLIC_ED_KEY;AUTOMATIC_CHECKS;CHECK_INTERVAL;VERSION;SHA256;DIST_DIR" ""
        ${ARGN})
    if(NOT APPLE OR IOS OR PULP_IOS)
        message(STATUS "Pulp: pulp_add_sparkle(${target}) ignored: Sparkle is macOS-only")
        return()
    endif()
    if(NOT TARGET ${target})
        message(FATAL_ERROR "pulp_add_sparkle: no target named ${target}")
    endif()
    get_target_property(_type ${target} TYPE)
    get_target_property(_bundle ${target} MACOSX_BUNDLE)
    if(NOT _type STREQUAL "EXECUTABLE" OR NOT _bundle)
        message(FATAL_ERROR
            "pulp_add_sparkle: ${target} is not a macOS app bundle. Sparkle belongs "
            "in the standalone .app only, never in an AU/VST3/CLAP plug-in bundle.")
    endif()
    if(NOT ARG_FEED_URL)
        message(FATAL_ERROR "pulp_add_sparkle: FEED_URL is required")
    endif()
    if(NOT ARG_FEED_URL MATCHES "^(https://|file://)")
        message(FATAL_ERROR
            "pulp_add_sparkle: FEED_URL must be https:// (or file:// for a local "
            "practice feed); got ${ARG_FEED_URL}")
    endif()
    if(NOT ARG_PUBLIC_ED_KEY MATCHES "^[A-Za-z0-9+/]{43}=$")
        message(FATAL_ERROR
            "pulp_add_sparkle: PUBLIC_ED_KEY must be the 44-character base64 Ed25519 "
            "public key (generate_keys -p). Never pass the private key here.")
    endif()
    if(DEFINED ARG_AUTOMATIC_CHECKS AND NOT ARG_AUTOMATIC_CHECKS STREQUAL "")
        if(ARG_AUTOMATIC_CHECKS)
            set(_auto "YES")
        else()
            set(_auto "NO")
        endif()
    endif()
    if(ARG_CHECK_INTERVAL AND NOT ARG_CHECK_INTERVAL MATCHES "^[0-9]+$")
        message(FATAL_ERROR "pulp_add_sparkle: CHECK_INTERVAL must be whole seconds")
    endif()

    pulp_resolve_sparkle_distribution(_dist
        VERSION "${ARG_VERSION}" SHA256 "${ARG_SHA256}" DIST_DIR "${ARG_DIST_DIR}")

    find_program(PULP_DITTO ditto REQUIRED)
    find_program(PULP_PLUTIL plutil REQUIRED)
    find_program(PULP_CODESIGN codesign REQUIRED)

    set(_fw_dir "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks")
    set(_fw "${_fw_dir}/Sparkle.framework")
    set(_plist "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Info.plist")

    target_link_options(${target} PRIVATE
        "-F${_dist}" "-Wl,-needed_framework,Sparkle")
    set_property(TARGET ${target} APPEND PROPERTY BUILD_RPATH "@executable_path/../Frameworks")
    set_property(TARGET ${target} APPEND PROPERTY INSTALL_RPATH "@executable_path/../Frameworks")

    set(_commands
        COMMAND "${CMAKE_COMMAND}" -E rm -rf "${_fw}"
        COMMAND "${CMAKE_COMMAND}" -E make_directory "${_fw_dir}"
        COMMAND "${PULP_DITTO}" "${_dist}/Sparkle.framework" "${_fw}")
    if(NOT ARG_KEEP_XPC_SERVICES)
        list(APPEND _commands
            COMMAND "${CMAKE_COMMAND}" -E rm -rf
                "${_fw}/Versions/B/XPCServices" "${_fw}/XPCServices"
            # Removing nested code invalidates the upstream ad hoc seal.
            COMMAND "${PULP_CODESIGN}" --force --sign - --options runtime "${_fw}")
    endif()
    list(APPEND _commands
        COMMAND "${PULP_PLUTIL}" -replace SUFeedURL -string "${ARG_FEED_URL}" "${_plist}"
        COMMAND "${PULP_PLUTIL}" -replace SUPublicEDKey -string "${ARG_PUBLIC_ED_KEY}" "${_plist}")
    if(DEFINED _auto)
        list(APPEND _commands
            COMMAND "${PULP_PLUTIL}" -replace SUEnableAutomaticChecks -bool ${_auto} "${_plist}")
    endif()
    if(ARG_CHECK_INTERVAL)
        list(APPEND _commands
            COMMAND "${PULP_PLUTIL}" -replace SUScheduledCheckInterval -integer
                ${ARG_CHECK_INTERVAL} "${_plist}")
    endif()
    add_custom_command(TARGET ${target} POST_BUILD ${_commands}
        COMMENT "Embedding Sparkle.framework in ${target}"
        VERBATIM)

    set_target_properties(${target} PROPERTIES
        PULP_SPARKLE_FEED_URL "${ARG_FEED_URL}"
        PULP_SPARKLE_DIST_DIR "${_dist}")
endfunction()
