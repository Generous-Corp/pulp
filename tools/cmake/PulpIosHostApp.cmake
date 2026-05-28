# PulpIosHostApp.cmake — Generate an iOS SwiftUI HostApp .app bundle that
# embeds a Pulp AUv3 .appex extension (Phase iOS-B of the AUv3 iOS
# validation plan, planning/2026-05-24-auv3-ios-validation.md).
#
# Why this exists:
#   pulp_add_ios_auv3() (PulpAuv3.cmake) builds the .appex extension only.
#   iOS App-Store policy requires AUv3 extensions to ship inside a
#   containing HostApp .app bundle; without the HostApp wrapper there is
#   nothing for `xcrun simctl install` to install, and no Launch Services
#   anchor for PluginKit to discover the extension.
#
# Why a HostApp at all on iOS (vs macOS):
#   On macOS, AU v3 plug-ins are discovered system-wide via Launch Services
#   once any container .app is opened once. The container .app can be a
#   minimal Cocoa shell that does nothing (templates/auv3/HostMain.m.in).
#   On iOS, the only way to install an AUv3 .appex is to ship it inside a
#   HostApp .app; even GarageBand's AU extension list reads from the
#   AVAudioUnitComponentManager registry, which only learns about an
#   extension after the user installs (and ideally launches) the HostApp.
#   The HostApp doubles as a smoke-test target — its SwiftUI shell
#   (templates/ios-auv3/HostApp/) instantiates the AUv3 itself so
#   "does it load + render parameters" is verifiable without GarageBand.
#
# Public function: pulp_add_ios_host_app(target ...)
#
#   pulp_add_ios_host_app(PulpSineSynth_HostApp
#       AUV3_EXTENSION  PulpSineSynth_AUv3         # required, must exist
#       BUNDLE_ID       com.pulp.examples.sinesynth.host
#       NAME            "PulpSineSynth Host"       # optional, defaults to target
#       VERSION         0.1.0                       # optional, defaults to "1.0.0"
#       DEPLOYMENT_TARGET 16.4                      # optional, defaults to 16.0
#       SOURCES         HostApp/PulpHostApp.swift HostApp/ContentView.swift
#           # optional — defaults to the shipped templates/ios-auv3/HostApp/ pair
#   )
#
#   The HostApp's Info.plist mirrors the AudioComponentDescription from the
#   AUv3 extension so AVAudioUnitComponentManager.components(matching:)
#   filters return the right plug-in inside the HostApp's ContentView.
#
#   After build, the .app sits at $<TARGET_BUNDLE_DIR:${target}>. Install
#   onto the Simulator via:
#     xcrun simctl install booted "$<TARGET_BUNDLE_DIR:${target}>"
#     xcrun simctl launch  booted <BUNDLE_ID>
#   (see docs/getting-started/ios-deployment.md for the full recipe.)
#
# Guarded by CMAKE_SYSTEM_NAME == iOS (PULP_IOS). Non-iOS configures emit
# a STATUS message and skip — keeps the same example CMakeLists.txt
# usable on macOS without forcing a fork.

function(pulp_add_ios_host_app target)
    if(NOT PULP_IOS)
        message(STATUS
            "pulp_add_ios_host_app(${target}): skipped — requires "
            "-DCMAKE_SYSTEM_NAME=iOS. Configure the iOS HostApp from an "
            "iOS Simulator or iOS Device CMake configure.")
        return()
    endif()

    cmake_parse_arguments(HOST
        ""
        "AUV3_EXTENSION;BUNDLE_ID;NAME;VERSION;DEPLOYMENT_TARGET"
        "SOURCES"
        ${ARGN}
    )

    foreach(_req IN ITEMS AUV3_EXTENSION BUNDLE_ID)
        if(NOT HOST_${_req})
            message(FATAL_ERROR
                "pulp_add_ios_host_app(${target}): ${_req} is required.")
        endif()
    endforeach()

    if(NOT TARGET ${HOST_AUV3_EXTENSION})
        message(FATAL_ERROR
            "pulp_add_ios_host_app(${target}): AUV3_EXTENSION target "
            "'${HOST_AUV3_EXTENSION}' does not exist. Call "
            "pulp_add_ios_auv3(...) first so the extension target is "
            "available for embedding.")
    endif()

    if(NOT HOST_NAME)
        set(HOST_NAME "${target}")
    endif()
    if(NOT HOST_VERSION)
        set(HOST_VERSION "1.0.0")
    endif()
    if(NOT HOST_DEPLOYMENT_TARGET)
        # Prefer the user-supplied root-build deployment target if set
        # (e.g. -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4). Default to 16.3 —
        # the minimum the iPhoneSimulator26.x SDK's libc++ allows for
        # `std::format` / `std::to_chars` floating-point overloads
        # (see PulpAuv3.cmake for the matching deployment-target floor).
        if(CMAKE_OSX_DEPLOYMENT_TARGET)
            set(HOST_DEPLOYMENT_TARGET "${CMAKE_OSX_DEPLOYMENT_TARGET}")
        else()
            set(HOST_DEPLOYMENT_TARGET "16.3")
        endif()
    endif()

    # Default sources: the shipped SwiftUI HostApp template. Plug-in
    # authors override SOURCES when they want a custom UI; the defaults
    # are enough for "does my AUv3 instantiate + parameter-render".
    if(NOT HOST_SOURCES)
        if(NOT _PULP_IOS_AUV3_TEMPLATE_DIR)
            message(FATAL_ERROR
                "pulp_add_ios_host_app(${target}): no SOURCES provided and "
                "_PULP_IOS_AUV3_TEMPLATE_DIR is unset. Either pass SOURCES "
                "explicitly or define -DPULP_IOS_AUV3_TEMPLATE_DIR=... .")
        endif()
        set(HOST_SOURCES
            "${_PULP_IOS_AUV3_TEMPLATE_DIR}/HostApp/PulpHostApp.swift"
            "${_PULP_IOS_AUV3_TEMPLATE_DIR}/HostApp/ContentView.swift"
        )
    endif()

    # ── Read AudioComponentDescription from the AUv3 extension ──────────
    # The AUv3 extension's Info.plist already encodes
    # manufacturer/subtype/type/version — read them back via target
    # properties set by _pulp_add_auv3_ios so the HostApp's Info.plist
    # describes the same component. Reading the codes via a single
    # source of truth (the extension target) prevents the
    # extension-vs-host descriptor drift that silently breaks AVAudioUnit
    # instantiation. We stash them as INTERFACE-style props at
    # _pulp_add_auv3_ios time; see PulpAuv3.cmake.
    get_target_property(_au_manufacturer  ${HOST_AUV3_EXTENSION} PULP_AUV3_MANUFACTURER_CODE)
    get_target_property(_au_subtype       ${HOST_AUV3_EXTENSION} PULP_AUV3_SUBTYPE_CODE)
    get_target_property(_au_type          ${HOST_AUV3_EXTENSION} PULP_AUV3_AU_TYPE)
    get_target_property(_au_version_int   ${HOST_AUV3_EXTENSION} PULP_AUV3_VERSION_INT)
    get_target_property(_au_plugin_name   ${HOST_AUV3_EXTENSION} PULP_AUV3_PLUGIN_NAME)
    get_target_property(_au_manufacturer_name ${HOST_AUV3_EXTENSION} PULP_AUV3_MANUFACTURER_NAME)

    foreach(_var IN ITEMS _au_manufacturer _au_subtype _au_type _au_version_int _au_plugin_name _au_manufacturer_name)
        if("${${_var}}" STREQUAL "${_var}-NOTFOUND")
            message(FATAL_ERROR
                "pulp_add_ios_host_app(${target}): AUv3 extension target "
                "'${HOST_AUV3_EXTENSION}' is missing the ${_var} property. "
                "This usually means PulpAuv3.cmake's _pulp_add_auv3_ios did "
                "not stash the AudioComponentDescription on the target. "
                "Make sure you called pulp_add_ios_auv3(...) before "
                "pulp_add_ios_host_app(...).")
        endif()
    endforeach()

    # ── Generate HostApp Info.plist ─────────────────────────────────────
    set(PLUGIN_NAME                 "${HOST_NAME}")
    set(PLUGIN_BUNDLE_ID            "${HOST_BUNDLE_ID}")
    set(PLUGIN_VERSION              "${HOST_VERSION}")
    set(PLUGIN_MANUFACTURER         "${_au_manufacturer_name}")
    set(PLUGIN_MANUFACTURER_CODE    "${_au_manufacturer}")
    set(PLUGIN_SUBTYPE_CODE         "${_au_subtype}")
    set(PLUGIN_AU_TYPE              "${_au_type}")
    set(PLUGIN_AU_VERSION_INT       "${_au_version_int}")
    set(PLUGIN_AU_PLUGIN_NAME       "${_au_plugin_name}")
    set(PLUGIN_DEPLOYMENT_TARGET    "${HOST_DEPLOYMENT_TARGET}")

    set(_host_plist_template "${_PULP_IOS_AUV3_TEMPLATE_DIR}/HostApp/Info.plist.in")
    set(_host_entitlements_template "${_PULP_IOS_AUV3_TEMPLATE_DIR}/HostApp/Entitlements.plist.in")

    if(NOT EXISTS "${_host_plist_template}")
        message(FATAL_ERROR
            "pulp_add_ios_host_app(${target}): missing template at "
            "${_host_plist_template}. The HostApp Info.plist template ships "
            "in templates/ios-auv3/HostApp/Info.plist.in alongside "
            "PulpHostApp.swift.")
    endif()

    set(_host_plist_out         "${CMAKE_BINARY_DIR}/AUv3/${target}-Info.plist")
    set(_host_entitlements_out  "${CMAKE_BINARY_DIR}/AUv3/${target}.entitlements")

    configure_file("${_host_plist_template}" "${_host_plist_out}" @ONLY)
    if(EXISTS "${_host_entitlements_template}")
        configure_file("${_host_entitlements_template}" "${_host_entitlements_out}" @ONLY)
    endif()

    # ── Enable Swift language (only if HostApp has .swift sources) ──────
    # Most HostApps will — the default template is SwiftUI. enable_language
    # is idempotent so re-calls across multiple pulp_add_ios_host_app
    # invocations are safe.
    set(_has_swift FALSE)
    foreach(_src IN LISTS HOST_SOURCES)
        if(_src MATCHES "\\.swift$")
            set(_has_swift TRUE)
            break()
        endif()
    endforeach()
    if(_has_swift)
        enable_language(Swift)
    endif()

    # ── HostApp .app target ─────────────────────────────────────────────
    add_executable(${target} MACOSX_BUNDLE ${HOST_SOURCES})

    set_target_properties(${target} PROPERTIES
        BUNDLE                              TRUE
        MACOSX_BUNDLE_INFO_PLIST            "${_host_plist_out}"
        RUNTIME_OUTPUT_DIRECTORY            "${CMAKE_BINARY_DIR}/AUv3"
        OUTPUT_NAME                         "${HOST_NAME}"
        XCODE_ATTRIBUTE_PRODUCT_BUNDLE_IDENTIFIER "${HOST_BUNDLE_ID}"
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${HOST_DEPLOYMENT_TARGET}"
        # SwiftUI HostApps need Swift compile + the AVFoundation/
        # AudioToolbox modules at link time. enable_language(Swift) above
        # registers the Swift compiler; the Xcode generator emits the
        # PRODUCT_TYPE + Swift settings automatically for .app bundles
        # that contain .swift sources.
        XCODE_ATTRIBUTE_SWIFT_VERSION       "5.0"
        XCODE_ATTRIBUTE_CLANG_ENABLE_MODULES "YES"
    )

    # When the target has Swift sources, tell CMake explicitly that Swift
    # is the linker language — otherwise the Xcode generator can fall
    # back to the C path when no .c/.cpp/.mm sources are present and
    # emit "CMake can not determine linker language" diagnostics during
    # the generate step.
    if(_has_swift)
        set_target_properties(${target} PROPERTIES LINKER_LANGUAGE Swift)
    endif()

    if(EXISTS "${_host_entitlements_out}")
        set_target_properties(${target} PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS "${_host_entitlements_out}")
    endif()

    # CoreAudioTypes is part of AudioToolbox on iOS — it is NOT a
    # top-level framework (unlike macOS where it ships under
    # /System/Library/Frameworks/CoreAudioTypes.framework). Linking it
    # standalone fails with "framework 'CoreAudioTypes' not found" on
    # the iPhoneSimulator26.x SDK. AudioToolbox is sufficient for the
    # AudioComponentDescription / AVAudioUnitComponent types the HostApp
    # uses.
    target_link_libraries(${target} PRIVATE
        "-framework UIKit"
        "-framework SwiftUI"
        "-framework AVFoundation"
        "-framework AudioToolbox"
    )

    # ── Embed the .appex into Contents/PlugIns ──────────────────────────
    # iOS .app bundles use a flat Contents-less layout
    # (.app/PlugIns/<name>.appex) — Apple's iOS bundle layout drops the
    # Contents/ directory that macOS bundles use. CMake's BUNDLE_DIR
    # generator expression resolves to the correct platform layout
    # automatically.
    #
    # Embed strategy mirrors the macOS AUv3 container in PulpAuv3.cmake:
    # a custom target driven by the .appex output, so a re-build of the
    # extension forces a re-embed even when the HostApp itself didn't
    # relink. POST_BUILD-only would silently ship the old .appex when
    # only the extension changed.
    set(_host_bundle_dir "$<TARGET_BUNDLE_DIR:${target}>")
    set(_appex_src_dir   "$<TARGET_BUNDLE_DIR:${HOST_AUV3_EXTENSION}>")
    set(_appex_name      "${_au_plugin_name}.appex")
    set(_embed_sentinel  "${CMAKE_BINARY_DIR}/AUv3/${target}_hostapp_embed.stamp")

    add_custom_command(
        OUTPUT  "${_embed_sentinel}"
        DEPENDS ${HOST_AUV3_EXTENSION} ${target}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${_host_bundle_dir}/PlugIns"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${_host_bundle_dir}/PlugIns/${_appex_name}"
        COMMAND cp -RP "${_appex_src_dir}" "${_host_bundle_dir}/PlugIns/"
        COMMAND ${CMAKE_COMMAND} -E touch "${_embed_sentinel}"
        COMMENT "Embedding ${_appex_name} into ${HOST_NAME}.app (iOS HostApp)"
        VERBATIM
    )
    add_custom_target(${target}_Embed ALL DEPENDS "${_embed_sentinel}")

    add_dependencies(${target} ${HOST_AUV3_EXTENSION})

    message(STATUS
        "Pulp iOS HostApp: ${target} (bundle: ${HOST_BUNDLE_ID}, embeds: ${HOST_AUV3_EXTENSION})")
endfunction()
