# PulpInstallRules.cmake — SDK install + export rules.
#
# Extracted from the root CMakeLists.txt in the 2026-05 Phase 3 (C2)
# refactor. Owns `install()` calls, `export(TARGETS ...)`, the
# `sdk_build_type.txt` marker generation, PulpConfig + PulpConfigVersion
# write, and CMake helper-script installation.
#
# Called once from the root CMakeLists.txt after every subsystem
# subdir has been added (the install rules reference targets like
# pulp::view / pulp::format / pulp::ship that the subsystem
# CMakeLists.txt files define).
#
# Codex C2 risk callout: configure output + the installed SDK
# manifest must be byte-stable before/after.

# ── SDK Install Rules ──────────────────────────────────────────────────────
# cmake --install build --prefix <staging> produces a complete Pulp SDK
# Android builds use Gradle for packaging — skip CMake install rules.
if(ANDROID)
    return()
endif()
include(GNUInstallDirs)

# Core libraries for SDK packaging. Standalone projects need the same runtime
# surface as repo examples, so the SDK exports the view/standalone layers plus
# the bundled third-party pieces they depend on. Some targets, like
# `pulp-render`, are optional in smoke or non-GPU builds, so only export targets
# that were actually configured in this build tree.
set(PULP_SDK_TARGETS
    pulp-platform pulp-runtime pulp-events pulp-state
    pulp-audio pulp-midi pulp-signal pulp-format
    pulp-canvas pulp-view-core pulp-view-script pulp-view
    pulp-host pulp-standalone pulp-dsl
)

if(TARGET pulp-render)
    list(APPEND PULP_SDK_TARGETS pulp-render)
endif()

if(TARGET pulp-inspect)
    list(APPEND PULP_SDK_TARGETS pulp-inspect)
endif()

# pulp-canvas links pulp-bundled-fonts privately when Skia is on (#932).
# CMake exports the canvas target through PulpTargets, and refuses unless
# every PRIVATE-linked dependency is also in the export set.
if(TARGET pulp-bundled-fonts)
    list(APPEND PULP_SDK_TARGETS pulp-bundled-fonts)
endif()
# Optional color-emoji typeface bundle — only present when
# PULP_BUNDLE_NOTO_COLOR_EMOJI=ON (defaults ON for Linux/Android/headless,
# OFF for macOS/Windows release builds that prefer the platform emoji font).
if(TARGET pulp-bundled-noto-color-emoji)
    list(APPEND PULP_SDK_TARGETS pulp-bundled-noto-color-emoji)
endif()

foreach(_sdk_target IN LISTS PULP_SDK_TARGETS)
    string(REPLACE "pulp-" "" _sdk_export_name "${_sdk_target}")
    set_target_properties(${_sdk_target} PROPERTIES EXPORT_NAME "${_sdk_export_name}")
endforeach()

install(TARGETS ${PULP_SDK_TARGETS}
    EXPORT PulpTargets
    ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
    LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    INCLUDES DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
)

# Also install third-party targets that our exported targets depend on
if(PULP_HAS_VST3)
    install(TARGETS vst3-sdk
        EXPORT PulpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION external/vst3sdk
    )
endif()
if(PULP_HAS_CLAP)
    install(TARGETS clap EXPORT PulpTargets)
endif()
if(PULP_HAS_OBOE AND TARGET oboe)
    install(TARGETS oboe
        EXPORT PulpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
if(PULP_HAS_LV2)
    install(TARGETS lv2-headers EXPORT PulpTargets)
endif()
if(PULP_HAS_AUSDK)
    install(TARGETS ausdk
        EXPORT PulpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        INCLUDES DESTINATION external/AudioUnitSDK/include
    )
endif()
if(TARGET yogacore)
    set_target_properties(yogacore PROPERTIES
        EXPORT_NAME yogacore
        INTERFACE_INCLUDE_DIRECTORIES "$<BUILD_INTERFACE:${yoga_SOURCE_DIR}>;$<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>"
    )
    install(TARGETS yogacore
        EXPORT PulpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
    if(DEFINED yoga_SOURCE_DIR AND EXISTS "${yoga_SOURCE_DIR}/yoga")
        install(DIRECTORY "${yoga_SOURCE_DIR}/yoga"
            DESTINATION ${CMAKE_INSTALL_INCLUDEDIR}
        )
    endif()
endif()
if(TARGET hwy)
    install(TARGETS hwy
        EXPORT PulpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )
endif()
foreach(_mbed_target mbedcrypto p256m everest mbedx509 mbedtls)
    if(TARGET ${_mbed_target})
        install(TARGETS ${_mbed_target}
            EXPORT PulpTargets
            ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
            LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        )
    endif()
endforeach()
if(TARGET SDL3-static)
    set_target_properties(SDL3-static PROPERTIES EXPORT_NAME sdl3-static)
    install(TARGETS SDL3-static
        EXPORT PulpTargets
        ARCHIVE DESTINATION ${CMAKE_INSTALL_LIBDIR}
        LIBRARY DESTINATION ${CMAKE_INSTALL_LIBDIR}
        RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR}
    )
endif()
if(TARGET SDL3_Headers)
    set_target_properties(SDL3_Headers PROPERTIES EXPORT_NAME sdl3-headers)
    install(TARGETS SDL3_Headers EXPORT PulpTargets)
endif()

# Public headers for each SDK subsystem
foreach(subsystem platform runtime events state audio midi signal format canvas render view)
    set(_inc_dir "${CMAKE_CURRENT_SOURCE_DIR}/core/${subsystem}/include")
    if(EXISTS "${_inc_dir}")
        install(DIRECTORY "${_inc_dir}/pulp/"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pulp"
            FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
        )
    endif()
endforeach()

# SDK version file
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${PROJECT_VERSION}\n")
install(FILES "${CMAKE_BINARY_DIR}/version.txt" DESTINATION ".")

# SDK build-type marker (pulp-internal task #35).
#
# Records the CMAKE_BUILD_TYPE the SDK was built with so PulpConfig.cmake
# can refuse — or at least loudly warn — when a downstream plugin tries
# to consume a Debug-built Pulp SDK. Debug-built Skia + audio framework
# blow real-time deadlines on every reasonable host, so consumers who
# stumble into this on a local checkout deserve a clear "this SDK
# install is unusable for plugin work" message instead of a silently
# unusable plugin that drops audio.
#
# Multi-config generators (Visual Studio, Xcode) don't have a single
# CMAKE_BUILD_TYPE at configure time; the install step picks one via
# `cmake --install --config <Type>`. In that case CMAKE_BUILD_TYPE is
# empty here and the guard in PulpConfig.cmake.in falls back to
# "unknown" — which it interprets as "trust the user, do not warn".
file(WRITE "${CMAKE_BINARY_DIR}/sdk_build_type.txt"
     "${CMAKE_BUILD_TYPE}\n")
install(FILES "${CMAKE_BINARY_DIR}/sdk_build_type.txt" DESTINATION ".")

# CMake config files for find_package(Pulp)
install(EXPORT PulpTargets
    FILE PulpTargets.cmake
    NAMESPACE Pulp::
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp
)

include(CMakePackageConfigHelpers)
write_basic_package_version_file(
    "${CMAKE_BINARY_DIR}/PulpConfigVersion.cmake"
    VERSION ${PROJECT_VERSION}
    COMPATIBILITY SameMajorVersion
)

configure_package_config_file(
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpConfig.cmake.in"
    "${CMAKE_BINARY_DIR}/PulpConfig.cmake"
    INSTALL_DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp
)

install(FILES
    "${CMAKE_BINARY_DIR}/PulpConfig.cmake"
    "${CMAKE_BINARY_DIR}/PulpConfigVersion.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAAX.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAppIcon.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpEmbedData.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpFonts.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpGenerateMacIcns.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpGenerateWindowsIcon.ps1"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpLinkFontconfig.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpUtils.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpPluginFormats.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAuv3.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAppTargets.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpPlugin.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpPlatformConfig.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpPkgConfigImports.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpSdkGuards.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpWebGpuImportedTarget.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/FindSkia.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpInfoPlist.aax.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpInfoPlist.au.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpInfoPlist.vst3.in"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp
)

# pulp_add_binary_data resolves its Python encoder via
# ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/encode_binary_data.py — i.e.
# adjacent to whichever PulpUtils.cmake is sourced. That works in the
# source tree (tools/cmake/PulpUtils.cmake → tools/cmake/scripts/…) and
# must keep working in the installed tree (lib/cmake/Pulp/PulpUtils.cmake
# → lib/cmake/Pulp/scripts/…). Install the encoder alongside PulpUtils.cmake
# so find_package(Pulp) consumers can use the function (issue-905 follow-up).
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/scripts/encode_binary_data.py"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp/scripts
)

# Templates for standalone project scaffolding
install(DIRECTORY tools/templates/
    DESTINATION templates
    PATTERN "*.template"
)
install(DIRECTORY templates/ios-auv3
    DESTINATION templates
)

# Header-only third-party deps referenced by installed public headers
install(DIRECTORY "${choc_SOURCE_DIR}/choc"
    DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)

if(DEFINED SDL3_SOURCE_DIR AND EXISTS "${SDL3_SOURCE_DIR}/include/SDL3")
    install(DIRECTORY "${SDL3_SOURCE_DIR}/include/SDL3"
        DESTINATION external/SDL3/include
    )
    if(EXISTS "${SDL3_SOURCE_DIR}/include/build_config")
        install(DIRECTORY "${SDL3_SOURCE_DIR}/include/build_config"
            DESTINATION external/SDL3/include
        )
    endif()
endif()
if(DEFINED SDL3_BINARY_DIR AND EXISTS "${SDL3_BINARY_DIR}/include-revision/SDL3")
    install(DIRECTORY "${SDL3_BINARY_DIR}/include-revision/SDL3"
        DESTINATION external/SDL3/include
    )
endif()

# Format SDK headers (CLAP, LV2 — MIT/ISC compatible)
if(PULP_HAS_CLAP)
    install(DIRECTORY "${clap_SOURCE_DIR}/include/clap"
        DESTINATION external/clap/include
    )
endif()
if(PULP_HAS_LV2)
    install(DIRECTORY "${lv2_SOURCE_DIR}/include/lv2"
        DESTINATION external/lv2/include
    )
endif()
if(PULP_HAS_VST3)
    install(DIRECTORY
        "${VST3_SDK_DIR}/base"
        "${VST3_SDK_DIR}/pluginterfaces"
        "${VST3_SDK_DIR}/public.sdk"
        DESTINATION external/vst3sdk
        FILES_MATCHING PATTERN "*.h"
    )
    install(FILES
        "${VST3_SDK_DIR}/public.sdk/source/main/dllmain.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/main/linuxmain.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/main/macmain.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/main/moduleinit.cpp"
        "${VST3_SDK_DIR}/public.sdk/source/main/pluginfactory.cpp"
        DESTINATION external/vst3sdk/public.sdk/source/main
    )
endif()
if(PULP_HAS_AUSDK)
    install(DIRECTORY "${AUSDK_DIR}/include/AudioUnitSDK"
        DESTINATION external/AudioUnitSDK/include
    )
endif()

# Helper sources needed by installed-SDK plugin targets. These are shared by
# the unified PulpUtils.cmake implementation when it is used from find_package.
#
# au_view_controller_mac.mm is the macOS AUv3 principal-class view controller
# (PulpAUMacViewController), referenced by PulpAuv3.cmake's macOS framework
# target. Without it, an installed-SDK consumer that calls
# pulp_add_plugin(FORMATS AUv3 ...) on macOS fails at configure time because
# _PULP_FORMAT_SOURCE_DIR/au_view_controller_mac.mm doesn't exist in the SDK
# tree (only au_view_controller_ios.mm was being shipped). This is the SDK
# packaging fix tracked in the Linux/macOS Chainer gap-closure plan.
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/aax_runtime.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_adapter.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_audio_unit.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_entry.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_v2_cocoa_view.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_view_controller_ios.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_view_controller_mac.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/vst3_plug_view.cpp"
    DESTINATION src/pulp/format
)

if(PULP_HAS_WEBGPU AND DEFINED WEBGPU_RUNTIME_LIB AND TARGET webgpu)
    install(IMPORTED_RUNTIME_ARTIFACTS webgpu
        DESTINATION ${CMAKE_INSTALL_LIBDIR}
    )

    get_filename_component(_pulp_webgpu_lib_dir "${WEBGPU_RUNTIME_LIB}" DIRECTORY)
    get_filename_component(_pulp_webgpu_root "${_pulp_webgpu_lib_dir}" DIRECTORY)

    if(DEFINED FETCHCONTENT_SOURCE_DIR_WEBGPU
       AND EXISTS "${FETCHCONTENT_SOURCE_DIR_WEBGPU}/wgpu-native/include")
        install(DIRECTORY "${FETCHCONTENT_SOURCE_DIR_WEBGPU}/wgpu-native/include/"
            DESTINATION external/webgpu/include
        )
    endif()

    if(EXISTS "${_pulp_webgpu_root}/include")
        install(DIRECTORY "${_pulp_webgpu_root}/include/"
            DESTINATION external/webgpu/include
        )
    endif()

    # Install the wgpu_native import library alongside the runtime DLL.
    #
    # The WebGPU-distribution CMake sets WGPU_IMPLIB as a local variable
    # inside FetchWgpuNativePrecompiled.cmake, so by the time execution
    # returns to this file the variable is out of scope — the original
    # guard `if(DEFINED WGPU_IMPLIB ...)` always evaluated to FALSE and
    # the .lib was silently skipped from the SDK. That's why scaffolded
    # Windows plugins fail with "IMPORTED_IMPLIB not set for imported
    # target 'webgpu'" (see #94).
    #
    # Read the path from the imported target directly. Prefer the
    # config-specific property (some WebGPU-distribution versions only
    # populate IMPORTED_IMPLIB_RELEASE) and fall back to the plain
    # IMPORTED_IMPLIB.
    if(WIN32)
        set(_pulp_wgpu_implib_src "")
        get_target_property(_pulp_wgpu_implib_release webgpu IMPORTED_IMPLIB_RELEASE)
        get_target_property(_pulp_wgpu_implib_plain webgpu IMPORTED_IMPLIB)
        if(_pulp_wgpu_implib_release AND EXISTS "${_pulp_wgpu_implib_release}")
            set(_pulp_wgpu_implib_src "${_pulp_wgpu_implib_release}")
        elseif(_pulp_wgpu_implib_plain AND EXISTS "${_pulp_wgpu_implib_plain}")
            set(_pulp_wgpu_implib_src "${_pulp_wgpu_implib_plain}")
        elseif(DEFINED WGPU_IMPLIB AND EXISTS "${WGPU_IMPLIB}")
            # Legacy fallback for older WebGPU-distribution versions.
            set(_pulp_wgpu_implib_src "${WGPU_IMPLIB}")
        endif()
        if(_pulp_wgpu_implib_src)
            install(FILES "${_pulp_wgpu_implib_src}"
                DESTINATION ${CMAKE_INSTALL_LIBDIR}
            )
        else()
            message(WARNING
                "Pulp: could not locate wgpu_native.lib for the Windows SDK "
                "install. Downstream plugins will fail to link against webgpu. "
                "Check that the webgpu imported target has IMPORTED_IMPLIB set.")
        endif()
        unset(_pulp_wgpu_implib_src)
        unset(_pulp_wgpu_implib_release)
        unset(_pulp_wgpu_implib_plain)
    endif()
endif()

if(PULP_HAS_SKIA)
    install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/external/skia-build"
        DESTINATION external
    )
endif()
