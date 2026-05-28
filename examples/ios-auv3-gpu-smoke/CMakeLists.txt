# Phase iOS-D.1 — minimal 2D GPU AUv3 proof on iOS.
#
# The smallest possible AU effect whose view renders a single animated
# 2D primitive through Skia/Dawn on iOS, proving that the GPU plugin
# host (`core/view/platform/ios/plugin_view_host_ios.mm`) and the Dawn
# Metal surface path (`core/render/src/gpu_surface_dawn.cpp`) work
# end-to-end inside an AUv3 .appex.
#
# Build (Simulator):
#   cmake -S . -B build-ios-sim-gpu -G Xcode \
#     -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
#     -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
#     -DPULP_ENABLE_GPU=ON -DPULP_REQUIRE_GPU_FOR_SDK=ON
#   cmake --build build-ios-sim-gpu --target PulpGpuSmoke_HostApp_Embed \
#     --config Release -- -sdk iphonesimulator
#
# Validation log markers (search syslog/Xcode console):
#   AU iOS: view controller loaded, ... gpu=1
#   GpuSurface: created Metal surface from CAMetalLayer
#   GpuSurface: Dawn initialized
#   GpuSurface: backend_type=Metal
#
# See planning/2026-05-24-auv3-ios-validation.md Phase iOS-D and the
# Codex crosscheck at planning/2026-05-28-ios-d-gpu-auv3-crosscheck.md.

if(NOT IOS)
    return()
endif()

pulp_add_ios_auv3(
    NAME            PulpGpuSmoke
    BUNDLE_ID       com.danielraffel.pulpdev.gpusmoke.host.PulpGpuSmoke
    MANUFACTURER    Pulp
    MANUFACTURER_CODE  Pulp
    SUBTYPE_CODE    PgSm
    AU_TYPE         aufx            # effect (no MIDI required, no synth voice)
    VERSION         0.1.0
    SOURCES
        src/gpu_smoke.cpp
        src/gpu_smoke.hpp
        src/au_v3_entry.cpp
)

pulp_add_ios_host_app(PulpGpuSmoke_HostApp
    AUV3_EXTENSION    PulpGpuSmoke_AUv3
    BUNDLE_ID         com.danielraffel.pulpdev.gpusmoke.host
    NAME              "PulpGpuSmoke"
    VERSION           0.1.0
    DEPLOYMENT_TARGET 16.4
)
