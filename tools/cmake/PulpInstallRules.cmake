# PulpInstallRules.cmake — SDK install + export rules.
#
# Owns `install()` calls, `export(TARGETS ...)`, the
# `sdk_build_type.txt` marker generation, PulpConfig + PulpConfigVersion
# write, and CMake helper-script installation.
#
# Called once from the root CMakeLists.txt after every subsystem
# subdir has been added (the install rules reference targets like
# pulp::view / pulp::format / pulp::ship that the subsystem
# CMakeLists.txt files define).
#
# Configure output and the installed SDK manifest should remain byte-stable
# when install rules move between CMake modules.

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
    pulp-platform pulp-runtime pulp-timebase pulp-events pulp-state
    pulp-audio pulp-midi pulp-signal pulp-graph pulp-format
    pulp-osc pulp-canvas pulp-view-core pulp-view
    pulp-standalone pulp-dsl pulp-native-components
)

# pulp-view-script (the JS scripting + web-compat layer) is EXCLUDE_FROM_ALL
# when PULP_ENABLE_JS=OFF (native-only build), so it is never built. Exporting
# it unconditionally makes `cmake --install` abort on a missing archive
# (libpulp-view-script.a) — the exact native-only packaging path PULP_ENABLE_JS
# exists to enable. pulp-view PUBLIC-links view-script only in the JS-on path,
# so it is only required in the export set then.
if(PULP_ENABLE_JS)
    list(APPEND PULP_SDK_TARGETS pulp-view-script)
endif()

# pulp-signal-fft-backend is the optional multi-backend FFT facade. It's
# defined in core/signal/CMakeLists.txt and ships a public header
# (`pulp/signal/fft_backend.hpp`). Without this export, find_package(Pulp)
# consumers can include the header but cannot link the symbols.
if(TARGET pulp-signal-fft-backend)
    list(APPEND PULP_SDK_TARGETS pulp-signal-fft-backend)
endif()

# pulp-host is platform-conditional: iOS skips the subdirectory entirely
# (no plugin hosting on iOS), so only include it in the SDK export set on
# platforms that actually built it.
if(TARGET pulp-host)
    list(APPEND PULP_SDK_TARGETS pulp-host)
endif()

if(TARGET pulp-render)
    list(APPEND PULP_SDK_TARGETS pulp-render)
endif()

# pulp-gpu-audio is the GPU-agnostic real-time audio transport plus the optional
# GPU convolver/spectral nodes. The transport target is always built, but its
# GPU nodes (and the PUBLIC link to pulp::render / pulp::signal) only compile
# when GPU is enabled. Export it when configured so find_package(Pulp) consumers
# — GPU plugins that drive a GpuAudioTransport — get pulp::gpu-audio. Its PUBLIC
# deps (pulp::audio, pulp::runtime, and in the GPU path pulp::render + pulp::signal)
# are all already in the export set, so the export set stays self-consistent.
if(TARGET pulp-gpu-audio)
    list(APPEND PULP_SDK_TARGETS pulp-gpu-audio)
endif()

if(TARGET pulp-inspect)
    list(APPEND PULP_SDK_TARGETS pulp-inspect)
endif()

# pulp-canvas links pulp-bundled-fonts privately when Skia is on.
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

# Public headers for each SDK subsystem. Keep platform-conditional surfaces
# aligned with the targets exported above: iOS deliberately has no pulp-host,
# so it must not receive orphaned desktop-host headers either.
set(_pulp_sdk_header_subsystems
    platform runtime timebase events state audio midi signal graph format osc canvas
    render view gpu_audio native-components dsl
)
if(TARGET pulp-host)
    list(APPEND _pulp_sdk_header_subsystems host)
endif()
foreach(subsystem IN LISTS _pulp_sdk_header_subsystems)
    set(_inc_dir "${CMAKE_CURRENT_SOURCE_DIR}/core/${subsystem}/include")
    if(EXISTS "${_inc_dir}")
        install(DIRECTORY "${_inc_dir}/pulp/"
            DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/pulp"
            FILES_MATCHING PATTERN "*.hpp" PATTERN "*.h"
        )
    endif()
endforeach()
unset(_pulp_sdk_header_subsystems)

# SDK license + third-party attribution.
#
# The rules above install third-party headers and archives into the SDK tree —
# VST3, CLAP, AudioUnitSDK, LV2, Yoga, Highway, mbedTLS, SDL3, SheenBidi. That
# is redistribution, and MIT conditions it on shipping the copyright and
# permission notice with "all copies or substantial portions of the Software"
# (Apache-2.0 §4 likewise requires the license text and any NOTICE). Without
# these two files the installed SDK carried none: a `cmake --install` tree held
# 218 files of third-party code and zero attribution, and every plugin that
# vendors the SDK inherited that.
#
# Installed at the SDK root, next to version.txt, so they are visible the
# moment the tree is unpacked rather than buried in share/doc.
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/LICENSE.md"
    "${CMAKE_CURRENT_SOURCE_DIR}/NOTICE.md"
    DESTINATION "."
)

# SDK version file
file(WRITE "${CMAKE_BINARY_DIR}/version.txt" "${PROJECT_VERSION}\n")
install(FILES "${CMAKE_BINARY_DIR}/version.txt" DESTINATION ".")

# SDK build-type marker.
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
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpMidiTuning.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpPluginMetadata.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpPluginFormats.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpPortable.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpBundleRelocatable.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAuv3.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpIosHostApp.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpAppTargets.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpPlugin.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpPlatformConfig.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpMinOs.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpPkgConfigImports.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpSdkGuards.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpWebGpuImportedTarget.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/FindSkia.cmake"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpInfoPlist.aax.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpInfoPlist.au.in"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/PulpInfoPlist.vst3.in"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp
)

# Ship the min-OS floor data next to PulpMinOs.cmake so a find_package(Pulp)
# consumer resolves the SAME floor Pulp's own build did. Without this file the
# consumer-side PulpMinOs.cmake finds no min_os.json and cannot pin, so the
# plugin would silently inherit the build host's OS floor.
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/deps/min_os.json"
    DESTINATION ${CMAKE_INSTALL_LIBDIR}/cmake/Pulp
)

# pulp_add_binary_data resolves its Python encoder via
# ${CMAKE_CURRENT_FUNCTION_LIST_DIR}/scripts/encode_binary_data.py — i.e.
# adjacent to whichever PulpUtils.cmake is sourced. That works in the
# source tree (tools/cmake/PulpUtils.cmake → tools/cmake/scripts/…) and
# must keep working in the installed tree (lib/cmake/Pulp/PulpUtils.cmake
# → lib/cmake/Pulp/scripts/…). Install the encoder alongside PulpUtils.cmake
# so find_package(Pulp) consumers can use the function.
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/scripts/encode_binary_data.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/scripts/check_portable_binary.py"
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/cmake/scripts/check_bundle_relocatable.py"
    # G3 universal-build arch/signature gate. pulp_validate_bundle_architectures
    # (PulpBundleRelocatable.cmake) resolves this next to itself, so ship it in
    # lib/cmake/Pulp/scripts too — the authoritative copy lives at
    # tools/scripts/check_bundle_architectures.py.
    "${CMAKE_CURRENT_SOURCE_DIR}/tools/scripts/check_bundle_architectures.py"
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
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/aax_midi_node.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/aax_effect_gui.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_adapter.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_audio_unit.h"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_entry.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_v2_cocoa_view.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_view_controller_ios.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/au_view_controller_mac.mm"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/format/src/vst3_plug_view.cpp"
    DESTINATION src/pulp/format
)

# Optional MIDI tuning provider wrappers used by installed-SDK consumers via
# pulp_enable_midi_tuning_provider(). The third-party packages remain opt-in
# through `pulp add`; these tiny wrapper sources are Pulp-owned glue.
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/midi/src/mts_esp_tuning.cpp"
    "${CMAKE_CURRENT_SOURCE_DIR}/core/midi/src/scala_tuning.cpp"
    DESTINATION src/pulp/midi
)

# Linux Skia compatibility source used by FindSkia.cmake when a prebuilt Skia
# archive references Chromium raw_ptr / PartitionAlloc support symbols without
# shipping a PartitionAlloc archive. Installed SDK consumers need the same
# source because they link the installed Skia archives through find_package(Pulp).
install(FILES
    "${CMAKE_CURRENT_SOURCE_DIR}/core/canvas/src/skia_chromium_raw_ptr_compat.cpp"
    DESTINATION src/pulp/canvas
)

# macOS view/window/accessibility Objective-C cluster. _pulp_apply_view_mac_objc_suffix()
# (PulpUtils.cmake) compiles a per-binary copy of these into each shipped plug-in /
# app so two Pulp binaries in one host don't register colliding ObjC class names.
# It probes `src/pulp/view/platform/mac` in an installed SDK, so ship the cluster
# there — otherwise an installed-SDK consumer silently falls back to pulp-view-core's
# shared fixed-name copies and the cross-plug-in class collision returns. The
# private headers (pulp_mac_objc_names.h + the window_host_mac split headers) ship
# alongside because the .mm include them by relative path.
if(APPLE)
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/pulp_mac_objc_names.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/window_host_mac.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/window_host_mac_capture.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/window_host_mac_geometry.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/window_host_mac_text_input.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/window_host_mac_capture.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/window_host_mac_internal.hpp"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/window_host_mac_view.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/plugin_view_host_mac.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/plugin_view_host_mac_text_input.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/drag_drop_mac.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/accessibility_mac.mm"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/view/platform/mac/text_accessibility_macos.mm"
        DESTINATION src/pulp/view/platform/mac
    )
    # Shared macOS render ObjC source (metal_surface_mac.mm), compiled per-binary
    # by _pulp_apply_view_mac_objc_suffix() so its PulpMetalView (the GPU-surface
    # NSView, renamed to PulpMetalSurfaceView) doesn't collide across plug-ins. The
    # helper probes src/pulp/render in an installed SDK; ship the source + the
    # render names header it includes there.
    install(FILES
        "${CMAKE_CURRENT_SOURCE_DIR}/core/render/src/pulp_render_objc_names.h"
        "${CMAKE_CURRENT_SOURCE_DIR}/core/render/src/metal_surface_mac.mm"
        DESTINATION src/pulp/render
    )
endif()

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
    # target 'webgpu'".
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
    # Ship the Skia tree the build actually linked against — including the
    # prebuilt static libs (build/<plat>-gpu/lib/Release/*.a) consumers need to
    # link Pulp::view/render. FindSkia sets SKIA_DIR to the in-tree default on a
    # normal checkout, or to an out-of-tree cache when -DSKIA_DIR/$SKIA_DIR is
    # used (e.g. a worktree whose external/skia-build holds only headers). Using
    # the resolved SKIA_DIR makes the installed SDK self-contained either way.
    if(SKIA_DIR AND EXISTS "${SKIA_DIR}")
        install(DIRECTORY "${SKIA_DIR}/" DESTINATION external/skia-build)
    else()
        install(DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/external/skia-build"
            DESTINATION external
        )
    endif()
endif()
