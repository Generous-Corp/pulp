# The shared GPU device layer (gpu-runtime): VellumGpuRuntime::{DeviceApi,
# Bootstrap, Device, SkiaLogBridge}.
#
# gpu-runtime is a self-contained subproject of the Vellum repository with no
# Pulp or Vellum dependency: the Dawn device, the image's one Dawn proc-table
# coordinator, and the Skia/Dawn renderer lock. Pulp's device targets forward
# to it on macOS, so GPU audio and rendering in one plug-in share one device
# layer and one proc table.
#
# Two ways in, the first that applies wins:
#   PULP_GPU_RUNTIME_PREFIX   an extracted vellum-gpu-runtime archive (or any
#                             install of the package), found by exact version;
#   source mode (default)     the pinned Vellum commit below, built from its
#                             gpu-runtime/ directory against Pulp's own Skia.
#
# Either way the package checks Dawn against its renderer lock, which must name
# the Skia asset Pulp resolved (tools/deps/audit.py --strict asserts the same).
#
# Sets PULP_HAS_GPU_RUNTIME when the targets exist.

set(PULP_GPU_RUNTIME_PREFIX "" CACHE PATH
    "An installed vellum-gpu-runtime package; empty builds the pinned source")
# Pinned in tools/deps/manifest.json ("vellum-gpu-runtime"); keep them equal.
set(PULP_GPU_RUNTIME_COMMIT "14bd4c5e430bc8da6b4204f1daa8a1bdbc6b7f3a")
set(PULP_GPU_RUNTIME_VERSION "0.1.10")

# OFF builds Pulp's own device sources on macOS too, the layer every other
# platform still uses; it keeps that mode compilable where it can gate.
option(PULP_USE_GPU_RUNTIME "On macOS, forward the GPU device layer to gpu-runtime" ON)

include("${CMAKE_CURRENT_LIST_DIR}/PulpGpuRuntimeRecord.cmake")

set(PULP_HAS_GPU_RUNTIME FALSE)
if(NOT PULP_USE_GPU_RUNTIME OR NOT (APPLE AND NOT PULP_IOS) OR NOT PULP_SKIA_PROVIDES_DAWN
   OR NOT SKIA_DIR)
    return()
endif()

if(PULP_GPU_RUNTIME_PREFIX)
    pulp_check_gpu_runtime_record("${PULP_GPU_RUNTIME_PREFIX}" "${PULP_GPU_RUNTIME_COMMIT}")
    set(GPU_RUNTIME_SKIA_DIR "${SKIA_DIR}")
    find_package(VellumGpuRuntime ${PULP_GPU_RUNTIME_VERSION} EXACT CONFIG REQUIRED
        COMPONENTS Device SkiaLogBridge
        PATHS "${PULP_GPU_RUNTIME_PREFIX}" NO_DEFAULT_PATH)
else()
    include(FetchContent)
    FetchContent_Declare(
        vellum_gpu_runtime
        GIT_REPOSITORY https://github.com/Generous-Corp/vellum.git
        GIT_TAG ${PULP_GPU_RUNTIME_COMMIT}
        SOURCE_SUBDIR gpu-runtime
    )
    set(GPU_RUNTIME_SKIA_DIR "${SKIA_DIR}" CACHE PATH "" FORCE)
    set(GPU_RUNTIME_REQUIRE_DEVICE ON CACHE BOOL "" FORCE)
    set(GPU_RUNTIME_BUILD_TESTS OFF CACHE BOOL "" FORCE)
    # The package installs beside Pulp's SDK; the SDK already ships the Skia
    # provider tree its Dawn comes from, so the Dawn library is not copied.
    set(GPU_RUNTIME_INSTALL ON CACHE BOOL "" FORCE)
    set(GPU_RUNTIME_INSTALL_DAWN OFF CACHE BOOL "" FORCE)
    set(GPU_RUNTIME_BENCHMARK ${PULP_BENCHMARK} CACHE BOOL "" FORCE)
    set(GPU_RUNTIME_HOST_OWNS_DAWN_PROCS ${PULP_DAWN_PROCS_HOST_OWNED} CACHE BOOL "" FORCE)
    FetchContent_MakeAvailable(vellum_gpu_runtime)
    get_directory_property(_pulp_gpu_runtime_version
        DIRECTORY "${vellum_gpu_runtime_SOURCE_DIR}/gpu-runtime" DEFINITION PROJECT_VERSION)
    if(NOT _pulp_gpu_runtime_version STREQUAL PULP_GPU_RUNTIME_VERSION)
        message(FATAL_ERROR
            "Pulp: gpu-runtime at ${PULP_GPU_RUNTIME_COMMIT} is version "
            "${_pulp_gpu_runtime_version}, not the pinned ${PULP_GPU_RUNTIME_VERSION}")
    endif()
endif()

foreach(_pulp_gpu_runtime_target DeviceApi Bootstrap Device SkiaLogBridge)
    if(NOT TARGET VellumGpuRuntime::${_pulp_gpu_runtime_target})
        message(FATAL_ERROR
            "Pulp: gpu-runtime did not provide VellumGpuRuntime::${_pulp_gpu_runtime_target}")
    endif()
endforeach()
# The benchmark hooks change gpu-runtime's GpuCompute vtable, and an image links
# one device library, so the flavour is global and must match PULP_BENCHMARK
# (itself a global definition). Its DeviceApi target records the flavour it
# was built with; a prefix package built the other way is refused here rather
# than at the first benchmark consumer.
get_target_property(_pulp_gpu_runtime_api_defs VellumGpuRuntime::DeviceApi
    INTERFACE_COMPILE_DEFINITIONS)
if(_pulp_gpu_runtime_api_defs AND "${_pulp_gpu_runtime_api_defs}" MATCHES "GPU_RUNTIME_BENCHMARK")
    set(_pulp_gpu_runtime_benchmark ON)
else()
    set(_pulp_gpu_runtime_benchmark OFF)
endif()
if(PULP_BENCHMARK)
    set(_pulp_benchmark ON)
else()
    set(_pulp_benchmark OFF)
endif()
if(NOT _pulp_gpu_runtime_benchmark STREQUAL _pulp_benchmark)
    message(FATAL_ERROR
        "Pulp: PULP_BENCHMARK is ${_pulp_benchmark}, but gpu-runtime was built with "
        "GPU_RUNTIME_BENCHMARK ${_pulp_gpu_runtime_benchmark}; the benchmark flavour "
        "changes the GpuCompute vtable, so the two must agree")
endif()
unset(_pulp_gpu_runtime_api_defs)
unset(_pulp_gpu_runtime_benchmark)
unset(_pulp_benchmark)
set(PULP_HAS_GPU_RUNTIME TRUE)
# Every consumer must agree on what pulp::render::GpuSurface names
# (pulp/platform/gpu_fwd.hpp), so the switch travels with pulp::platform.
# The headers the switch selects (<gpu_runtime/...>) travel with it: DeviceApi
# is header-only, so this adds include directories and no second copy of any
# gpu-runtime archive. gpu-runtime-mode-consistency checks that every target
# carrying the define also links DeviceApi. The property is appended directly:
# target_link_libraries() from outside pulp-platform's own directory records
# a directory-scope marker beside the entry, which every reader of the link
# interface would then see.
target_compile_definitions(pulp-platform INTERFACE PULP_GPU_RUNTIME=1)
set_property(TARGET pulp-platform APPEND PROPERTY
    INTERFACE_LINK_LIBRARIES VellumGpuRuntime::DeviceApi)
