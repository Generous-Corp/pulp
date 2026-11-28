# pulp-vellum-renderer3d: the native scene3d renderer on Vellum's GPU device
# layer. The Dawn device comes from gpu-runtime (VellumGpuRuntime::Device,
# Vellum's framework-free device package that Pulp already builds for GPU
# audio); the renderer only borrows its device, queue and instance handles.
# The package does not link pulp::render. It stays in Pulp, not in Vellum,
# because it renders pulp::scene data, and Vellum cannot depend on Pulp.
# Core never links this package.
if(NOT PULP_HAS_GPU_RUNTIME OR NOT TARGET VellumGpuRuntime::Device)
    message(FATAL_ERROR
        "PULP_ENABLE_PULP_VELLUM_RENDERER3D=ON needs gpu-runtime (macOS, PULP_USE_GPU_RUNTIME=ON and a Skia slice that carries Dawn)")
endif()
if(NOT PULP_HAS_SKIA OR NOT TARGET skia::skia)
    message(FATAL_ERROR
        "PULP_ENABLE_PULP_VELLUM_RENDERER3D=ON needs the Skia/Dawn renderer archive gpu-runtime's Dawn comes from")
endif()
if(NOT TARGET pulp::scene)
    message(FATAL_ERROR
        "PULP_ENABLE_PULP_VELLUM_RENDERER3D=ON needs pulp::scene; configure with PULP_ENABLE_SCENE3D=ON")
endif()

add_library(pulp-vellum-renderer3d STATIC
    src/renderer3d.cpp
    src/renderer3d_frame.cpp
    src/renderer3d_analysis.cpp
    src/renderer3d_materials.cpp
    src/renderer3d_scene.cpp
    src/renderer3d_scene_pass.cpp)
add_library(pulp::vellum-renderer3d ALIAS pulp-vellum-renderer3d)
target_include_directories(pulp-vellum-renderer3d
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE ${SKIA_INCLUDE_DIRS} "${SKIA_INCLUDE_DIRS}/dawn")
target_compile_features(pulp-vellum-renderer3d PUBLIC cxx_std_20)
# The scene shader, embedded (and validated where the host can run Dawn) at
# build time.
include("${PULP_ROOT_DIR}/tools/cmake/PulpWgslShaders.cmake")
pulp_add_wgsl_shaders(pulp-vellum-renderer3d
    FILES shaders/renderer3d_scene.wgsl
    NAMESPACE pulp::vellum_renderer3d::shaders)
# skia::skia carries libdawn_combined, the one Dawn in the image: gpu-runtime's
# device and every wgpu call here bind to it. Skia itself encodes the PNG.
target_link_libraries(pulp-vellum-renderer3d
    PUBLIC pulp::scene
    PRIVATE VellumGpuRuntime::Device skia::skia)
