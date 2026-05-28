# PulpAuv3.cmake — Audio Unit v3 helpers.
#
# Two AU v3 packaging shapes:
#
# 1. iOS app extension (PULP_IOS) — single .appex inside the iOS HostApp.
#    The .appex carries the AU code directly. Same layout that's shipped
#    via pulp_add_ios_auv3() since Phase 3.
#
# 2. macOS framework-inside-container (Phase 3.5) — Apple's "Creating
#    custom audio effects" sample architecture. The AU code lives in a
#    separate .framework bundle; the .appex is a stub that links the
#    framework via AudioComponentBundle. Both are embedded in a tiny
#    .app shell so Launch Services + PlugInKit discover the extension.
#    Required because Apple's docs explicitly say "the extension's main
#    binary cannot be dynamically loaded into another app, so all
#    executable AU code must live in a separate framework bundle."
#    iPlug2 ships the same architecture.
#
# Per-plugin macOS AU v3 targets created:
#   ${target}_AUv3Framework  — SHARED FRAMEWORK with the AU code
#   ${target}_AUv3           — stub .appex (links framework)
#   ${target}_AUv3Host       — container .app embedding both
#
# Functions:
#   _pulp_add_auv3                    — internal: per-target setup (dispatches by platform)
#   _pulp_add_auv3_macos_framework    — internal: macOS framework target
#   _pulp_add_auv3_macos_appex        — internal: macOS stub .appex
#   _pulp_add_auv3_macos_host         — internal: macOS container .app
#   _pulp_add_auv3_ios                — internal: iOS monolithic .appex (legacy shape)
#   pulp_add_ios_auv3                 — public: scaffold the iOS app + extension
function(_pulp_add_auv3 target name bundle_id version manufacturer category plugin_code manufacturer_code accepts_midi)
    if(NOT APPLE)
        return()
    endif()

    # Map category + accepts_midi to AU type and tag. Same routing logic as
    # _pulp_add_au — AU v3 hosts also require aumf for effects that want MIDI.
    if(category STREQUAL "Instrument")
        set(au_type "aumu")
        set(au_tag "Synthesizer")
    elseif(category STREQUAL "MidiEffect")
        set(au_type "aumi")
        set(au_tag "MIDI")
    elseif(accepts_midi)
        set(au_type "aumf")
        set(au_tag "Effects")
    else()
        set(au_type "aufx")
        set(au_tag "Effects")
    endif()

    # Compute integer version (major * 65536 + minor * 256 + patch).
    string(REPLACE "." ";" _ver_parts "${version}")
    list(GET _ver_parts 0 _ver_major)
    list(GET _ver_parts 1 _ver_minor)
    list(GET _ver_parts 2 _ver_patch)
    math(EXPR _au_version_int "${_ver_major} * 65536 + ${_ver_minor} * 256 + ${_ver_patch}")

    # Find per-plugin AU v3 entry (convention: au_v3_entry.cpp in plugin
    # source dir). Uses PULP_AUV3_PLUGIN to register the processor factory
    # at static init.
    set(auv3_entry "")
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/au_v3_entry.cpp")
        set(auv3_entry "${CMAKE_CURRENT_SOURCE_DIR}/au_v3_entry.cpp")
    endif()

    if(PULP_IOS)
        _pulp_add_auv3_ios(${target}
            "${name}" "${bundle_id}" "${version}" "${manufacturer}"
            "${manufacturer_code}" "${plugin_code}"
            "${au_type}" "${au_tag}" "${_au_version_int}"
            "${auv3_entry}")
    else()
        _pulp_add_auv3_macos_framework(${target}
            "${name}" "${bundle_id}" "${version}" "${auv3_entry}")
        _pulp_add_auv3_macos_appex(${target}
            "${name}" "${bundle_id}" "${version}" "${manufacturer}"
            "${manufacturer_code}" "${plugin_code}"
            "${au_type}" "${au_tag}" "${_au_version_int}")
        _pulp_add_auv3_macos_host(${target}
            "${name}" "${bundle_id}" "${version}")
    endif()
endfunction()

# ── macOS framework: the real AU code lives here ──────────────────────────
function(_pulp_add_auv3_macos_framework target name bundle_id version auv3_entry)
    set(fw_target ${target}_AUv3Framework)
    set(fw_output_name ${name}AUv3Framework)

    # Sources: the per-plugin Core OBJECT lib + the platform-shared AU
    # adapter + macOS view controller + per-plugin AU v3 entry. NOTE: we
    # intentionally do NOT include au_entry.mm here. That file defined a
    # legacy AudioComponent C factory; AU v3 instead loads the principal
    # class (PulpAUMacViewController, which adopts AUAudioUnitFactory) via
    # _NSExtensionMain. iPlug2 follows the same convention.
    add_library(${fw_target} SHARED
        ${PULP_${target}_CORE_OBJECTS}
        ${_PULP_FORMAT_SOURCE_DIR}/au_adapter.mm
        ${_PULP_FORMAT_SOURCE_DIR}/au_view_controller_mac.mm
        ${auv3_entry}
    )

    target_link_libraries(${fw_target} PRIVATE
        ${target}_Core
        ${_PULP_FORMAT_TARGET}
        ${_PULP_VIEW_TARGET}
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework CoreAudioKit"
        "-framework Cocoa"
    )
    target_include_directories(${fw_target} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    _pulp_apply_macho_exports(${fw_target} "${fw_target}"
        "_OBJC_CLASS_$_PulpAUMacViewController"
        "_OBJC_METACLASS_$_PulpAUMacViewController"
        "_OBJC_CLASS_$_PulpAudioUnit"
        "_OBJC_METACLASS_$_PulpAudioUnit")

    # Framework Info.plist
    set(PLUGIN_NAME "${name}")
    set(PLUGIN_BUNDLE_ID "${bundle_id}")
    set(PLUGIN_VERSION "${version}")
    configure_file(
        "${_PULP_AUV3_TEMPLATE_DIR}/Framework-Info.plist.template"
        "${CMAKE_BINARY_DIR}/AUv3/${fw_target}-Info.plist"
        @ONLY
    )

    # Framework bundle properties. install_name lets the .appex find the
    # framework via @rpath at runtime. iPlug2's xcconfig uses the same
    # @rpath-relative install name on macOS.
    set_target_properties(${fw_target} PROPERTIES
        FRAMEWORK TRUE
        FRAMEWORK_VERSION A
        MACOSX_FRAMEWORK_IDENTIFIER "${bundle_id}.AUv3Framework"
        MACOSX_FRAMEWORK_INFO_PLIST "${CMAKE_BINARY_DIR}/AUv3/${fw_target}-Info.plist"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AUv3"
        OUTPUT_NAME "${fw_output_name}"
        MACOSX_RPATH TRUE
        BUILD_WITH_INSTALL_NAME_DIR TRUE
        INSTALL_NAME_DIR "@rpath"
    )

    if(COMMAND target_copy_webgpu_binaries)
        target_copy_webgpu_binaries(${fw_target})
    endif()
endfunction()

# ── macOS stub .appex: tiny shell linking the framework ────────────────────
function(_pulp_add_auv3_macos_appex target name bundle_id version manufacturer manufacturer_code plugin_code au_type au_tag au_version_int)
    set(appex_target ${target}_AUv3)

    # Generate the stub source. The .appex must contain at least one
    # source file so it has an executable; the source itself does nothing
    # — _NSExtensionMain (Apple-provided via Foundation) reads the
    # NSExtension Info.plist and loads the principal class from the
    # framework. iPlug2's IPlugAUv3Appex.m is the exact same shape.
    set(stub_src "${CMAKE_BINARY_DIR}/AUv3/${appex_target}_stub.mm")
    # Sanitize plugin name into a C identifier for the keep-alive symbol.
    string(MAKE_C_IDENTIFIER "${name}" PLUGIN_NAME_CSYM)
    set(PLUGIN_NAME "${name}")
    configure_file(
        "${_PULP_AUV3_TEMPLATE_DIR}/AppexStub.m.in"
        "${stub_src}"
        @ONLY
    )

    add_executable(${appex_target} MACOSX_BUNDLE ${stub_src})

    # Configure the .appex Info.plist (the existing macOS template,
    # already updated for the framework architecture).
    set(PLUGIN_BUNDLE_ID "${bundle_id}")
    set(PLUGIN_VERSION "${version}")
    set(PLUGIN_MANUFACTURER "${manufacturer}")
    set(PLUGIN_MANUFACTURER_CODE "${manufacturer_code}")
    set(PLUGIN_SUBTYPE_CODE "${plugin_code}")
    set(PLUGIN_AU_TYPE "${au_type}")
    set(PLUGIN_AU_TAG "${au_tag}")
    set(PLUGIN_AU_VERSION_INT "${au_version_int}")
    set(PULP_AUV3_PRINCIPAL_CLASS "PulpAUMacViewController")
    set(PULP_AUV3_EXTENSION_POINT "com.apple.AudioUnit-UI")
    configure_file(
        "${_PULP_AUV3_TEMPLATE_DIR}/Info.plist.template"
        "${CMAKE_BINARY_DIR}/AUv3/${appex_target}-Info.plist"
        @ONLY
    )
    configure_file(
        "${_PULP_AUV3_TEMPLATE_DIR}/Appex-Entitlements.plist.template"
        "${CMAKE_BINARY_DIR}/AUv3/${appex_target}.entitlements"
        @ONLY
    )

    # Bundle shape: .appex with app-extension product type. _NSExtensionMain
    # is the entry point (provided by Foundation; we set -e explicitly).
    # rpath: from .appex/Contents/MacOS/X → .app/Contents/Frameworks is 4
    # parent dirs up, NOT 2. iPlug2's newer CMake helper has this wrong
    # for macOS; this is the correct macOS appex rpath.
    set_target_properties(${appex_target} PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "appex"
        XCODE_PRODUCT_TYPE "com.apple.product-type.app-extension"
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/AUv3/${appex_target}-Info.plist"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AUv3"
        OUTPUT_NAME "${name}"
        PREFIX ""
        SUFFIX ""
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../../../../Frameworks"
    )
    target_link_options(${appex_target} PRIVATE
        "-e" "_NSExtensionMain"
        "-fapplication-extension"
    )

    target_link_libraries(${appex_target} PRIVATE
        ${target}_AUv3Framework
        "-framework Foundation"
    )

    # Force-load the framework so static initializers (PulpAUMacViewController
    # registration, processor factory) run when the host loads the .appex.
    # Apple's _NSExtensionMain reads NSExtensionPrincipalClass and uses
    # NSClassFromString to find PulpAUMacViewController; that lookup
    # requires the framework's Obj-C classes to be registered with the
    # runtime, which happens at framework load.
    target_link_options(${appex_target} PRIVATE
        "-Wl,-force_load,$<TARGET_FILE:${target}_AUv3Framework>")
endfunction()

# ── macOS container .app: holds the .appex + framework ────────────────────
function(_pulp_add_auv3_macos_host target name bundle_id version)
    set(host_target ${target}_AUv3Host)
    set(host_output_name "${name}")

    set(host_src "${CMAKE_BINARY_DIR}/AUv3/${host_target}_main.mm")
    set(PLUGIN_NAME "${name}")
    set(PLUGIN_BUNDLE_ID "${bundle_id}")
    set(PLUGIN_VERSION "${version}")
    configure_file(
        "${_PULP_AUV3_TEMPLATE_DIR}/HostMain.m.in"
        "${host_src}"
        @ONLY
    )
    configure_file(
        "${_PULP_AUV3_TEMPLATE_DIR}/Host-Info.plist.template"
        "${CMAKE_BINARY_DIR}/AUv3/${host_target}-Info.plist"
        @ONLY
    )
    configure_file(
        "${_PULP_AUV3_TEMPLATE_DIR}/Host-Entitlements.plist.template"
        "${CMAKE_BINARY_DIR}/AUv3/${host_target}.entitlements"
        @ONLY
    )

    add_executable(${host_target} MACOSX_BUNDLE ${host_src})

    set_target_properties(${host_target} PROPERTIES
        MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/AUv3/${host_target}-Info.plist"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AUv3"
        OUTPUT_NAME "${host_output_name}"
        BUILD_WITH_INSTALL_RPATH TRUE
        INSTALL_RPATH "@executable_path/../Frameworks"
    )
    target_link_libraries(${host_target} PRIVATE "-framework Cocoa")

    # Make the host depend on both pieces and copy them into the bundle.
    add_dependencies(${host_target} ${target}_AUv3Framework ${target}_AUv3)

    # Embed framework into Contents/Frameworks. Use cp -RP to preserve
    # symlinks (Apple frameworks use the Versions/A pattern).
    #
    # PROBLEM: a plain POST_BUILD only fires when the host target relinks.
    # A framework-only source edit (the common dev loop) doesn't relink
    # the host, so the .app stays stale and you sign+ship the OLD code.
    # FIX: drive the embed step from a custom target that depends on the
    # framework + appex outputs, and make the host target depend on IT.
    # The custom target's command runs whenever those upstream binaries
    # are newer than the embed sentinel.
    set(host_bundle "$<TARGET_BUNDLE_DIR:${host_target}>")
    set(fw_src "$<TARGET_BUNDLE_DIR:${target}_AUv3Framework>")
    set(appex_src "$<TARGET_BUNDLE_DIR:${target}_AUv3>")
    set(embed_sentinel "${CMAKE_BINARY_DIR}/AUv3/${target}_embed.stamp")

    # The sentinel depends on the upstream framework + appex outputs so
    # the embed step re-fires when either is rebuilt. We also re-run if
    # the host bundle was relinked (host_target itself is a dep) — that
    # catches the case where someone wiped Contents/Frameworks by hand
    # AND relinked the host; for the rarer "wiped by hand without
    # relink" case, just delete the sentinel and re-build.
    add_custom_command(
        OUTPUT "${embed_sentinel}"
        DEPENDS ${target}_AUv3Framework ${target}_AUv3 ${host_target}
        COMMAND ${CMAKE_COMMAND} -E make_directory "${host_bundle}/Contents/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${host_bundle}/Contents/PlugIns"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${host_bundle}/Contents/Frameworks/${name}AUv3Framework.framework"
        COMMAND ${CMAKE_COMMAND} -E rm -rf "${host_bundle}/Contents/PlugIns/${name}.appex"
        COMMAND cp -RP "${fw_src}" "${host_bundle}/Contents/Frameworks/"
        COMMAND cp -RP "${appex_src}" "${host_bundle}/Contents/PlugIns/"
        COMMAND ${CMAKE_COMMAND} -E touch "${embed_sentinel}"
        COMMENT "Embedding ${name}AUv3Framework + ${name}.appex into ${name}.app"
        VERBATIM
    )
    add_custom_target(${host_target}_Embed ALL DEPENDS "${embed_sentinel}")
endfunction()

# ── iOS monolithic .appex (legacy Phase 3 shape) ──────────────────────────
function(_pulp_add_auv3_ios target name bundle_id version manufacturer manufacturer_code plugin_code au_type au_tag au_version_int auv3_entry)
    # iOS App Extension target: MUST be add_executable (MH_EXECUTE) not
    # add_library MODULE (MH_BUNDLE). PluginKit posix_spawn's the .appex's
    # binary directly; a bundle Mach-O triggers ENOEXEC ("Exec format
    # error") at launch time, surfacing as OSStatus 4 from
    # AVAudioUnit.instantiate. Use NSExtensionMain as the entry point —
    # standard iOS App Extension convention.
    add_executable(${target}_AUv3
        ${PULP_${target}_CORE_OBJECTS}
        ${_PULP_FORMAT_SOURCE_DIR}/au_adapter.mm
        ${_PULP_FORMAT_SOURCE_DIR}/au_entry.mm
        ${_PULP_FORMAT_SOURCE_DIR}/au_view_controller_ios.mm
        ${auv3_entry}
    )

    target_link_libraries(${target}_AUv3 PRIVATE
        ${target}_Core
        ${_PULP_FORMAT_TARGET}
        ${_PULP_VIEW_TARGET}
        "-framework AudioToolbox"
        "-framework AVFoundation"
        "-framework CoreAudioKit"
        "-framework UIKit"
    )
    # Entry point: NSExtensionMain (provided by Foundation on iOS app
    # extensions). -e _NSExtensionMain replaces the default _main entry.
    target_link_options(${target}_AUv3 PRIVATE
        "-e" "_NSExtensionMain"
        "-fapplication-extension"
    )
    target_include_directories(${target}_AUv3 PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    _pulp_apply_macho_exports(${target}_AUv3 "${target}_AUv3"
        "_OBJC_CLASS_$_PulpAUViewController"
        "_OBJC_METACLASS_$_PulpAUViewController"
        "_OBJC_CLASS_$_PulpAudioUnit"
        "_OBJC_METACLASS_$_PulpAudioUnit")

    set(PLUGIN_NAME "${name}")
    set(PLUGIN_BUNDLE_ID "${bundle_id}")
    set(PLUGIN_VERSION "${version}")
    set(PLUGIN_MANUFACTURER "${manufacturer}")
    set(PLUGIN_MANUFACTURER_CODE "${manufacturer_code}")
    set(PLUGIN_SUBTYPE_CODE "${plugin_code}")
    set(PLUGIN_AU_TYPE "${au_type}")
    set(PLUGIN_AU_TAG "${au_tag}")
    set(PLUGIN_AU_VERSION_INT "${au_version_int}")
    set(PULP_AUV3_PRINCIPAL_CLASS "PulpAUViewController")
    set(PULP_AUV3_EXTENSION_POINT "com.apple.AudioUnit-UI")

    if(EXISTS "${_PULP_IOS_AUV3_TEMPLATE_DIR}/Info.plist.in")
        configure_file(
            "${_PULP_IOS_AUV3_TEMPLATE_DIR}/Info.plist.in"
            "${CMAKE_BINARY_DIR}/AUv3/${target}.appex/Info.plist"
            @ONLY
        )
        set_target_properties(${target}_AUv3 PROPERTIES
            MACOSX_BUNDLE_INFO_PLIST "${CMAKE_BINARY_DIR}/AUv3/${target}.appex/Info.plist"
        )
    endif()

    # Item 3.10 (macOS plan) — pick the right entitlements template
    # for the active iOS SDK. The Simulator and a real device both use
    # the application-groups entitlement to share preset storage with
    # the containing app, but signing requirements differ enough that
    # we keep two distinct template files so a teams that ships through
    # the App Store can adjust the device variant without affecting the
    # local Simulator dev loop. Mac Catalyst is deliberately excluded
    # per the macOS plan ("3.10 ... Mac Catalyst is deferred post-MVP").
    if(CMAKE_OSX_SYSROOT MATCHES "Simulator" OR
       CMAKE_OSX_SYSROOT MATCHES "iphonesimulator")
        set(_auv3_ios_entitlements_src
            "${_PULP_AUV3_TEMPLATE_DIR}/iOS-Simulator-Entitlements.plist.template")
    else()
        set(_auv3_ios_entitlements_src
            "${_PULP_AUV3_TEMPLATE_DIR}/iOS-Device-Entitlements.plist.template")
    endif()
    if(EXISTS "${_auv3_ios_entitlements_src}")
        configure_file(
            "${_auv3_ios_entitlements_src}"
            "${CMAKE_BINARY_DIR}/AUv3/${target}.entitlements"
            @ONLY
        )
        set_target_properties(${target}_AUv3 PROPERTIES
            XCODE_ATTRIBUTE_CODE_SIGN_ENTITLEMENTS
                "${CMAKE_BINARY_DIR}/AUv3/${target}.entitlements")
    endif()

    # iOS deployment target: prefer the user-supplied
    # CMAKE_OSX_DEPLOYMENT_TARGET (e.g. -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4)
    # so the .appex matches the rest of the build. Fall back to 16.3 — the
    # minimum the iPhoneSimulator26.x SDK's libc++ allows for `std::format`
    # / `std::to_chars` floating-point overloads (older targets fail with
    # "'to_chars' is unavailable: introduced in iOS 16.3 simulator" when
    # any TU instantiates std::format, e.g. core/runtime/log.hpp).
    if(CMAKE_OSX_DEPLOYMENT_TARGET)
        set(_pulp_ios_min "${CMAKE_OSX_DEPLOYMENT_TARGET}")
    else()
        set(_pulp_ios_min "16.3")
    endif()
    set_target_properties(${target}_AUv3 PROPERTIES
        BUNDLE TRUE
        BUNDLE_EXTENSION "appex"
        XCODE_PRODUCT_TYPE "com.apple.product-type.app-extension"
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AUv3"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/AUv3"
        OUTPUT_NAME "${name}"
        PREFIX ""
        XCODE_ATTRIBUTE_TARGETED_DEVICE_FAMILY "1,2"
        XCODE_ATTRIBUTE_IPHONEOS_DEPLOYMENT_TARGET "${_pulp_ios_min}"
        XCODE_ATTRIBUTE_WRAPPER_EXTENSION "appex"
        # Stash the AudioComponentDescription on the target so
        # pulp_add_ios_host_app(...) can read them back without
        # re-deriving from the Info.plist on disk. This is the
        # single source of truth for HostApp/Extension descriptor
        # parity — drift between them silently breaks
        # AVAudioUnitComponentManager.components(matching:).
        PULP_AUV3_PLUGIN_NAME       "${name}"
        PULP_AUV3_MANUFACTURER_NAME "${manufacturer}"
        PULP_AUV3_MANUFACTURER_CODE "${manufacturer_code}"
        PULP_AUV3_SUBTYPE_CODE      "${plugin_code}"
        PULP_AUV3_AU_TYPE           "${au_type}"
        PULP_AUV3_AU_TAG            "${au_tag}"
        PULP_AUV3_VERSION_INT       "${au_version_int}"
        PULP_AUV3_BUNDLE_ID         "${bundle_id}"
    )

    if(COMMAND target_copy_webgpu_binaries)
        target_copy_webgpu_binaries(${target}_AUv3)
    endif()
endfunction()

# ── pulp_add_ios_auv3 (public iOS helper, unchanged from Phase 3) ─────────
# Public helper for iOS AUv3 app-extension targets. See top-of-file
# header comment for the iOS vs macOS packaging split.
function(pulp_add_ios_auv3)
    cmake_parse_arguments(AUV3
        "ACCEPTS_MIDI"
        "NAME;BUNDLE_ID;MANUFACTURER;MANUFACTURER_CODE;SUBTYPE_CODE;AU_TYPE;VERSION"
        "SOURCES"
        ${ARGN}
    )

    if(NOT PULP_IOS)
        message(FATAL_ERROR
            "pulp_add_ios_auv3(${AUV3_NAME}): iOS-only helper. Configure with "
            "-DCMAKE_SYSTEM_NAME=iOS (optionally -DCMAKE_OSX_SYSROOT=iphonesimulator) "
            "before invoking it.")
    endif()

    foreach(_req IN ITEMS NAME BUNDLE_ID MANUFACTURER MANUFACTURER_CODE SUBTYPE_CODE VERSION)
        if(NOT AUV3_${_req})
            message(FATAL_ERROR "pulp_add_ios_auv3: ${_req} is required")
        endif()
    endforeach()

    string(LENGTH "${AUV3_MANUFACTURER_CODE}" _mfr_len)
    if(NOT _mfr_len EQUAL 4)
        message(FATAL_ERROR
            "pulp_add_ios_auv3(${AUV3_NAME}): MANUFACTURER_CODE must be exactly "
            "4 characters (got '${AUV3_MANUFACTURER_CODE}', length ${_mfr_len}).")
    endif()
    string(LENGTH "${AUV3_SUBTYPE_CODE}" _sub_len)
    if(NOT _sub_len EQUAL 4)
        message(FATAL_ERROR
            "pulp_add_ios_auv3(${AUV3_NAME}): SUBTYPE_CODE must be exactly "
            "4 characters (got '${AUV3_SUBTYPE_CODE}', length ${_sub_len}).")
    endif()

    if(NOT AUV3_AU_TYPE)
        set(AUV3_AU_TYPE "aufx")
    endif()
    if(AUV3_AU_TYPE STREQUAL "aumu")
        set(_category "Instrument")
    elseif(AUV3_AU_TYPE STREQUAL "aumi")
        set(_category "MidiEffect")
    elseif(AUV3_AU_TYPE STREQUAL "aufx")
        set(_category "Effect")
    else()
        message(FATAL_ERROR
            "pulp_add_ios_auv3(${AUV3_NAME}): AU_TYPE must be aumu, aumi, or "
            "aufx (got '${AUV3_AU_TYPE}').")
    endif()

    set(target "${AUV3_NAME}")

    if(AUV3_SOURCES)
        add_library(${target}_Core OBJECT ${AUV3_SOURCES})
        target_link_libraries(${target}_Core PUBLIC ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
        target_compile_definitions(${target}_Core PRIVATE
            PULP_PLUGIN_NAME="${AUV3_NAME}"
            PULP_BUNDLE_ID="${AUV3_BUNDLE_ID}"
            PULP_PLUGIN_VERSION="${AUV3_VERSION}"
        )
    else()
        add_library(${target}_Core INTERFACE)
        target_link_libraries(${target}_Core INTERFACE ${_PULP_FORMAT_TARGET})
        target_include_directories(${target}_Core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR})
    endif()
    set(PULP_${target}_CORE_OBJECTS "" CACHE INTERNAL "")

    if(AUV3_ACCEPTS_MIDI)
        set(_auv3_accepts_midi "TRUE")
    else()
        set(_auv3_accepts_midi "FALSE")
    endif()

    _pulp_add_auv3(${target}
        "${AUV3_NAME}"
        "${AUV3_BUNDLE_ID}"
        "${AUV3_VERSION}"
        "${AUV3_MANUFACTURER}"
        "${_category}"
        "${AUV3_SUBTYPE_CODE}"
        "${AUV3_MANUFACTURER_CODE}"
        "${_auv3_accepts_midi}"
    )

    if(TARGET ${target}_AUv3)
        target_link_libraries(${target}_AUv3 PRIVATE ${target}_Core)
    endif()

    message(STATUS "Pulp iOS AUv3: ${target} (type: ${AUV3_AU_TYPE}, subtype: ${AUV3_SUBTYPE_CODE})")
endfunction()
