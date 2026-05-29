# Phase iOS-D.2 — Chainer-style iOS AUv3 example.
#
# Builds:
#   - PulpChainerIos.appex   (AU v3 extension, MH_EXECUTE, NSExtensionMain)
#   - PulpChainerIos.app     (SwiftUI HostApp, embeds the .appex,
#                             mounts the AUv3 view via
#                             UIViewControllerRepresentable so the
#                             Skia/Dawn-rendered Chainer editor is
#                             visible inside the host window)
#
# Configure (Simulator):
#   cmake -S . -B build-ios-sim-chainer -G Xcode \
#     -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
#     -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
#     -DPULP_ENABLE_GPU=ON -DPULP_REQUIRE_GPU_FOR_SDK=ON
#   cmake --build build-ios-sim-chainer \
#     --target PulpChainerIos_HostApp_Embed --config Release -- -sdk iphonesimulator
#
# Validation log markers (search syslog/Xcode console):
#   AU iOS: view controller loaded, ... gpu=1
#   GpuSurface: backend_type=Metal
#   [chainer-ios] editor attached, awaiting FrameClock ticks
#   PULP_EDITOR_REQUESTED: requestViewController -> present
#   PULP_EDITOR_MOUNTED: AUv3 view controller attached to HostApp container
#
# See planning/2026-05-24-auv3-ios-validation.md Phase iOS-D.

if(NOT IOS)
    return()
endif()

pulp_add_ios_auv3(
    NAME            PulpChainerIos
    BUNDLE_ID       com.danielraffel.pulpdev.chainerios.host.PulpChainerIos
    MANUFACTURER    Pulp
    MANUFACTURER_CODE  Pulp
    SUBTYPE_CODE    Chnr
    AU_TYPE         aumu            # instrument
    VERSION         0.1.0
    SOURCES
        src/chainer_synth.cpp
        src/chainer_synth.hpp
        src/au_v3_entry.cpp
)

pulp_add_ios_host_app(PulpChainerIos_HostApp
    AUV3_EXTENSION    PulpChainerIos_AUv3
    BUNDLE_ID         com.danielraffel.pulpdev.chainerios.host
    NAME              "PulpChainerIos"
    VERSION           0.1.0
    DEPLOYMENT_TARGET 16.4
)
