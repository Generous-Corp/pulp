# PulpSparkle.cmake — embed Sparkle 2 in a macOS standalone app.
#
#   pulp_add_sparkle(MyPlugin_Standalone
#       FEED_URL      "https://github.com/me/app/releases/latest/download/appcast.xml"
#                     (https only; http:// is accepted on 127.0.0.1/localhost)
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
#   * writes SUFeedURL / SUPublicEDKey (and the optional keys) into the app's
#     Info.plist TEMPLATE at the end of configure (cmake_language(DEFER)), so
#     the keys survive every regeneration and do not depend on whether
#     pulp_declare_standalone_document_type() or pulp_app_icon() ran before or
#     after this call. (Editing the built plist after link would be undone by
#     the next reconfigure until the next relink.)
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

# Escape a value for a plist <string> that CMake configures again at generate
# time: `$` and `@` become character references so neither pass eats them.
function(_pulp_sparkle_plist_escape out_var text)
    string(REPLACE "&" "&amp;" text "${text}")
    string(REPLACE "<" "&lt;" text "${text}")
    string(REPLACE ">" "&gt;" text "${text}")
    string(REPLACE "@" "&#64;" text "${text}")
    string(REPLACE "$" "&#36;" text "${text}")
    set(${out_var} "${text}" PARENT_SCOPE)
endfunction()

# Deferred to the end of the top-level configure: derive the app's final
# Info.plist template (whatever MACOSX_BUNDLE_INFO_PLIST ended up as, or
# CMake's default) and insert the Sparkle keys before its closing </dict>.
function(_pulp_sparkle_finalize_plist target)
    get_target_property(_template ${target} MACOSX_BUNDLE_INFO_PLIST)
    if(NOT _template)
        set(_template "${CMAKE_ROOT}/Modules/MacOSXBundleInfo.plist.in")
    endif()
    if(NOT EXISTS "${_template}")
        message(FATAL_ERROR "pulp_add_sparkle: Info.plist template not found: ${_template}")
    endif()
    file(READ "${_template}" _text)
    if(_text MATCHES "<key>SUFeedURL</key>")
        message(FATAL_ERROR
            "pulp_add_sparkle: ${target}'s Info.plist template already declares SUFeedURL; "
            "set the feed in one place only")
    endif()
    get_target_property(_keys ${target} PULP_SPARKLE_PLIST_XML)
    string(FIND "${_text}" "</dict>" _close REVERSE)
    if(_close EQUAL -1)
        message(FATAL_ERROR "pulp_add_sparkle: no </dict> in ${_template}")
    endif()
    string(SUBSTRING "${_text}" 0 ${_close} _head)
    string(SUBSTRING "${_text}" ${_close} -1 _tail)
    set(_out "${CMAKE_BINARY_DIR}/PulpSparkle/${target}-Info.plist.in")
    file(WRITE "${_out}" "${_head}${_keys}${_tail}")
    set_target_properties(${target} PROPERTIES MACOSX_BUNDLE_INFO_PLIST "${_out}")
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
    # Sparkle 2 downloads feeds only over http(s): a file:// feed fails at run
    # time with "The download request URL must use http or https". Plain http is
    # accepted only on loopback, for a local rehearsal feed.
    if(NOT ARG_FEED_URL MATCHES "^https://" AND
       NOT ARG_FEED_URL MATCHES "^http://(127\\.0\\.0\\.1|localhost)(:[0-9]+)?/")
        message(FATAL_ERROR
            "pulp_add_sparkle: FEED_URL must be https:// (or http://127.0.0.1/... for a "
            "local practice feed; Sparkle refuses file://); got ${ARG_FEED_URL}")
    endif()
    # CMake regexes have no {n} quantifier: check the length separately.
    string(LENGTH "${ARG_PUBLIC_ED_KEY}" _key_len)
    if(NOT _key_len EQUAL 44 OR NOT ARG_PUBLIC_ED_KEY MATCHES "^[A-Za-z0-9+/]+=$")
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
    find_program(PULP_CODESIGN codesign REQUIRED)

    set(_fw_dir "$<TARGET_BUNDLE_CONTENT_DIR:${target}>/Frameworks")
    set(_fw "${_fw_dir}/Sparkle.framework")

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
    add_custom_command(TARGET ${target} POST_BUILD ${_commands}
        COMMENT "Embedding Sparkle.framework in ${target}"
        VERBATIM)

    _pulp_sparkle_plist_escape(_feed_xml "${ARG_FEED_URL}")
    _pulp_sparkle_plist_escape(_key_xml "${ARG_PUBLIC_ED_KEY}")
    set(_keys "\t<key>SUFeedURL</key>\n\t<string>${_feed_xml}</string>\n")
    string(APPEND _keys "\t<key>SUPublicEDKey</key>\n\t<string>${_key_xml}</string>\n")
    if(DEFINED _auto)
        if(_auto STREQUAL "YES")
            string(APPEND _keys "\t<key>SUEnableAutomaticChecks</key>\n\t<true/>\n")
        else()
            string(APPEND _keys "\t<key>SUEnableAutomaticChecks</key>\n\t<false/>\n")
        endif()
    endif()
    if(ARG_CHECK_INTERVAL)
        string(APPEND _keys
            "\t<key>SUScheduledCheckInterval</key>\n\t<integer>${ARG_CHECK_INTERVAL}</integer>\n")
    endif()
    get_target_property(_already ${target} PULP_SPARKLE_PLIST_XML)
    set_target_properties(${target} PROPERTIES
        PULP_SPARKLE_FEED_URL "${ARG_FEED_URL}"
        PULP_SPARKLE_DIST_DIR "${_dist}"
        PULP_SPARKLE_PLIST_XML "${_keys}")
    if(NOT _already)
        # A deferred call expands its arguments when it RUNS, by which time
        # ${target} is gone; EVAL bakes the target name in now.
        cmake_language(EVAL CODE
            "cmake_language(DEFER DIRECTORY [[${CMAKE_SOURCE_DIR}]] CALL _pulp_sparkle_finalize_plist [[${target}]])")
    endif()
endfunction()
