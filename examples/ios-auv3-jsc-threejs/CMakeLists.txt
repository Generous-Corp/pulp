# Three.js rotating-cube AUv3 demo on iPad.
#
# This is the AUv3 Three.js GPU demo: a near-silent AU effect plug-in whose
# editor pane loads `three.webgpu.js` (via the build-time IIFE bundle) inside
# JSC, then paints a rotating cube through Pulp's Dawn/Metal surface stack.
#
# The plumbing this exercises:
#   - `PluginViewHost::gpu_surface()` + `WidgetBridge::attach_gpu_surface`
#   - `pulp::view::threejs_iife_source()` — NSBundle loader for
#     `<appex>/threejs/three.iife.js`
#   - `tools/scripts/bundle_threejs_for_jsc.mjs` — build-time ESM →
#     IIFE wrapper
#   - `__gpuCanvasConfigureImpl` / `__gpuCanvasDescribeCurrentTextureImpl`
#     exposing a `presentable` flag so JS draws hit the visible swapchain
#   - Runtime log markers `PULP_WEBGPU_BRIDGE: ready`,
#     `PULP_WEBGPU_BRIDGE: ... ok (presentable=true)`,
#     `PULP_WEBGPU_BRIDGE: queue.submit ok`
#
# Build (Simulator):
#   cmake -S . -B build-ios-sim-three -G Xcode \
#     -DCMAKE_SYSTEM_NAME=iOS -DCMAKE_OSX_SYSROOT=iphonesimulator \
#     -DCMAKE_OSX_ARCHITECTURES=arm64 -DCMAKE_OSX_DEPLOYMENT_TARGET=16.4 \
#     -DPULP_ENABLE_GPU=ON -DPULP_REQUIRE_GPU_FOR_SDK=ON
#   cmake --build build-ios-sim-three \
#     --target PulpThreeJsDemo_HostApp_Embed \
#     --config Release -- -sdk iphonesimulator
#
# Validation log markers (search syslog/Xcode console):
#   AU iOS: view controller loaded, ... gpu=1
#   GpuSurface: backend_type=Metal
#   [plugin-gpu-host] GpuSurface attached to WidgetBridge via ScriptedUiSession (iOS AUv3)
#   PULP_THREE_DEMO: IIFE loaded (N bytes)
#   PULP_THREE_DEMO: scene script loaded (N bytes)
#   PULP_THREEJS: globalThis.THREE available (NN exports)
#   PULP_THREE_SHIM: ready
#   PULP_THREE_SHIM: webgpu-renderer-present
#   PULP_THREE_RENDER: first frame submitted
#
# Use this target to verify that the AUv3 editor can load the bundled
# Three.js IIFE, attach Pulp's presentable GPU surface, and submit a
# WebGPU frame on device.

if(NOT IOS)
    return()
endif()

# Pull in the Three.js source even though `PULP_BUILD_TESTS=OFF` (the
# iOS preset forces tests off, so the root `PulpDependencies.cmake`
# `if(PULP_BUILD_TESTS AND PULP_ENABLE_GPU)` gate that normally arms
# `threejs_SOURCE_DIR` does not fire on iOS). The `_pulp_add_auv3_ios`
# POST_BUILD step that copies `three.iife.js` into the .appex is gated
# on `PULP_HAS_THREEJS AND DEFINED threejs_SOURCE_DIR` — both have to
# be set before `pulp_add_ios_auv3(...)` runs so the bundler attaches
# to OUR target.
#
# Setting them in this subdirectory's scope keeps the cost local: other
# iOS examples (synth, chainer, gpu-smoke) never see the Three.js
# checkout because their CMakeLists never enters this block.
if(NOT PULP_HAS_THREEJS OR NOT DEFINED threejs_SOURCE_DIR)
    include(FetchContent)
    pulp_register_fetchcontent_source(threejs REF 077dd13c0e869d9f3dbe55875686f920367de457)
    FetchContent_Declare(
        threejs
        GIT_REPOSITORY https://github.com/mrdoob/three.js.git
        GIT_TAG 077dd13c0e869d9f3dbe55875686f920367de457
    )
    FetchContent_MakeAvailable(threejs)
    set(PULP_HAS_THREEJS TRUE)
    message(STATUS
        "Pulp iOS-D.3c (ios-auv3-jsc-threejs): three.js sources fetched "
        "for IIFE bundling at ${threejs_SOURCE_DIR}")
endif()

pulp_add_ios_auv3(
    NAME            PulpThreeJsDemo
    BUNDLE_ID       com.danielraffel.pulpdev.threejsdemo.host.PulpThreeJsDemo
    MANUFACTURER    Pulp
    MANUFACTURER_CODE  Pulp
    SUBTYPE_CODE    PtJs
    AU_TYPE         aufx           # effect (silent pass-through); audio path is not the focus
    VERSION         0.1.0
    SOURCES
        src/threejs_demo.mm
        src/threejs_demo.hpp
        src/au_v3_entry.cpp
)

# Bundle the JS scene next to Pulp's `three.iife.js` so the runtime
# loader (`PulpThreeJsDemoProcessor::create_view()`) can resolve it via
# the same `[NSBundle bundleForClass:[PulpAUViewController class]]`
# pattern that `pulp::view::threejs_iife_source()` uses. The scene
# lives at `<appex>/threejs/scene.js`; reading it as a sibling of
# `three.iife.js` keeps the resource layout consistent.
if(TARGET PulpThreeJsDemo_AUv3)
    set(_scene_src "${CMAKE_CURRENT_SOURCE_DIR}/js/scene.js")
    set(_scene_out "$<TARGET_BUNDLE_DIR:PulpThreeJsDemo_AUv3>/threejs/scene.js")
    add_custom_command(TARGET PulpThreeJsDemo_AUv3 POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E make_directory
                "$<TARGET_BUNDLE_DIR:PulpThreeJsDemo_AUv3>/threejs"
        COMMAND ${CMAKE_COMMAND} -E copy "${_scene_src}" "${_scene_out}"
        COMMENT "iOS-D.3c: copy scene.js into PulpThreeJsDemo.appex/threejs/"
        VERBATIM
    )
endif()

pulp_add_ios_host_app(PulpThreeJsDemo_HostApp
    AUV3_EXTENSION    PulpThreeJsDemo_AUv3
    BUNDLE_ID         com.danielraffel.pulpdev.threejsdemo.host
    NAME              "PulpThreeJsDemo"
    VERSION           0.1.0
    DEPLOYMENT_TARGET 16.4
)
