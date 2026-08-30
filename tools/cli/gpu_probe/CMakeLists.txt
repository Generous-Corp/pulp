file(READ "${CMAKE_SOURCE_DIR}/docs/status/gpu-recipes.yaml"
    PULP_GPU_RECIPE_CATALOG_JSON)
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/docs/status/gpu-recipes.yaml")
file(MAKE_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/generated")
configure_file(
    "${CMAKE_SOURCE_DIR}/tools/cli/gpu_recipe_catalog_data.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/gpu_probe_recipe_catalog_data.h"
    @ONLY)

add_library(pulp-tool-gpu-probe-model STATIC
    src/probe_result.cpp
    src/probe_result_json.cpp)
add_library(pulp::tool-gpu-probe-model ALIAS pulp-tool-gpu-probe-model)
target_compile_features(pulp-tool-gpu-probe-model PUBLIC cxx_std_20)
target_include_directories(pulp-tool-gpu-probe-model
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE
        $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
        "${CMAKE_CURRENT_BINARY_DIR}/generated")
if(PULP_ENABLE_GPU)
    target_compile_definitions(pulp-tool-gpu-probe-model PUBLIC
        PULP_GPU_PROBE_GPU_CALLABLE=1)
else()
    target_compile_definitions(pulp-tool-gpu-probe-model PUBLIC
        PULP_GPU_PROBE_GPU_CALLABLE=0)
endif()
if(PULP_ENABLE_GPU AND PULP_ENABLE_SCENE3D)
    target_compile_definitions(pulp-tool-gpu-probe-model PUBLIC
        PULP_GPU_PROBE_RENDERER3D_CALLABLE=1)
else()
    target_compile_definitions(pulp-tool-gpu-probe-model PUBLIC
        PULP_GPU_PROBE_RENDERER3D_CALLABLE=0)
endif()
if(PULP_ENABLE_GPU AND PULP_ENABLE_JS AND PULP_HAS_THREEJS
        AND PULP_JS_ENGINE STREQUAL "v8")
    target_compile_definitions(pulp-tool-gpu-probe-model PUBLIC
        PULP_GPU_PROBE_THREEJS_CALLABLE=1)
else()
    target_compile_definitions(pulp-tool-gpu-probe-model PUBLIC
        PULP_GPU_PROBE_THREEJS_CALLABLE=0)
endif()

if(PULP_ENABLE_GPU)
    add_library(pulp-tool-gpu-dpr-measurement STATIC
        src/dpr_measurement.cpp
        src/dpr_measurement_session.cpp)
    target_compile_features(pulp-tool-gpu-dpr-measurement PUBLIC cxx_std_20)
    target_include_directories(pulp-tool-gpu-dpr-measurement
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE $<BUILD_INTERFACE:${choc_SOURCE_DIR}>)
    target_link_libraries(pulp-tool-gpu-dpr-measurement
        PUBLIC pulp::render pulp::runtime pulp::state pulp::view)
    if(PULP_HAS_THREEJS)
        file(SHA256 "${threejs_SOURCE_DIR}/build/three.webgpu.js"
            PULP_DPR_THREEJS_WEBGPU_SHA256)
        file(SHA256 "${threejs_SOURCE_DIR}/build/three.core.js"
            PULP_DPR_THREEJS_CORE_SHA256)
        target_compile_definitions(pulp-tool-gpu-dpr-measurement PRIVATE
            PULP_DPR_HAS_THREEJS=1
            PULP_DPR_THREEJS_SOURCE_DIR="${threejs_SOURCE_DIR}"
            PULP_DPR_THREEJS_WEBGPU_SHA256="${PULP_DPR_THREEJS_WEBGPU_SHA256}"
            PULP_DPR_THREEJS_CORE_SHA256="${PULP_DPR_THREEJS_CORE_SHA256}")
    else()
        target_compile_definitions(pulp-tool-gpu-dpr-measurement PRIVATE
            PULP_DPR_HAS_THREEJS=0)
    endif()

    add_executable(pulp-gpu-dpr-native-measurement EXCLUDE_FROM_ALL
        src/dpr_measurement_main.cpp)
    target_link_libraries(pulp-gpu-dpr-native-measurement PRIVATE
        pulp-tool-gpu-dpr-measurement)

    file(SHA256 "${CMAKE_SOURCE_DIR}/core/render/src/renderer3d.cpp"
        PULP_RENDERER3D_TRANSLATION_UNIT_SHA256)
    file(READ "${CMAKE_SOURCE_DIR}/test/fixtures/scene3d/renderer3d-goldens.json"
        PULP_RENDERER3D_GOLDENS_JSON)
    string(JSON PULP_RENDERER3D_METAL_FINGERPRINT GET
        "${PULP_RENDERER3D_GOLDENS_JSON}" entries 0 fingerprint)
    configure_file(src/renderer3d_oracle.hpp.in
        ${CMAKE_CURRENT_BINARY_DIR}/generated/renderer3d_oracle.hpp @ONLY)

    add_library(pulp-tool-gpu-probe-recipes STATIC
        src/native_recipes.cpp)
    add_library(pulp::tool-gpu-probe-recipes ALIAS pulp-tool-gpu-probe-recipes)
    target_compile_features(pulp-tool-gpu-probe-recipes PUBLIC cxx_std_20)
    target_include_directories(pulp-tool-gpu-probe-recipes
        PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        PRIVATE ${CMAKE_CURRENT_BINARY_DIR}/generated)
    target_link_libraries(pulp-tool-gpu-probe-recipes
        PUBLIC pulp::tool-gpu-probe-model
        PRIVATE pulp::gpu-audio pulp::render pulp::runtime)
    if(PULP_HAS_THREEJS)
        file(SHA256 "${threejs_SOURCE_DIR}/build/three.webgpu.js"
            PULP_GPU_PROBE_THREEJS_WEBGPU_SHA256)
        file(SHA256 "${threejs_SOURCE_DIR}/build/three.core.js"
            PULP_GPU_PROBE_THREEJS_CORE_SHA256)
        target_compile_definitions(pulp-tool-gpu-probe-recipes PRIVATE
            PULP_GPU_PROBE_HAS_THREEJS=1
            PULP_GPU_PROBE_THREEJS_SOURCE_DIR="${threejs_SOURCE_DIR}"
            PULP_GPU_PROBE_THREEJS_WEBGPU_SHA256="${PULP_GPU_PROBE_THREEJS_WEBGPU_SHA256}"
            PULP_GPU_PROBE_THREEJS_CORE_SHA256="${PULP_GPU_PROBE_THREEJS_CORE_SHA256}")
        target_link_libraries(pulp-tool-gpu-probe-recipes PRIVATE pulp::view pulp::state)
    else()
        target_compile_definitions(pulp-tool-gpu-probe-recipes PRIVATE
            PULP_GPU_PROBE_HAS_THREEJS=0)
    endif()
    if(PULP_ENABLE_SCENE3D)
        target_link_libraries(pulp-tool-gpu-probe-recipes PRIVATE pulp::scene)
    endif()

    if(PULP_ENABLE_SCENE3D)
        add_executable(pulp-gpu-probe-native-acceptance EXCLUDE_FROM_ALL
            src/native_acceptance.cpp)
        target_link_libraries(pulp-gpu-probe-native-acceptance PRIVATE
            pulp::tool-gpu-probe-recipes)
    endif()

    add_executable(pulp-gpu-probe-stft-native-acceptance EXCLUDE_FROM_ALL
        src/stft_native_acceptance.cpp)
    target_link_libraries(pulp-gpu-probe-stft-native-acceptance PRIVATE
        pulp::tool-gpu-probe-recipes)
endif()

if(PULP_BUILD_TESTS)
    add_executable(pulp-test-gpu-probe-result
        ${CMAKE_SOURCE_DIR}/test/test_gpu_probe_result.cpp)
    target_link_libraries(pulp-test-gpu-probe-result PRIVATE
        pulp::tool-gpu-probe-model Catch2::Catch2WithMain)
    add_test(NAME gpu-probe-result COMMAND pulp-test-gpu-probe-result)

    if(PULP_ENABLE_GPU)
        add_executable(pulp-test-gpu-dpr-native-measurement
            ${CMAKE_SOURCE_DIR}/test/test_gpu_dpr_native_measurement.cpp)
        target_link_libraries(pulp-test-gpu-dpr-native-measurement PRIVATE
            pulp-tool-gpu-dpr-measurement Catch2::Catch2WithMain)
        add_test(NAME gpu-dpr-native-measurement
            COMMAND pulp-test-gpu-dpr-native-measurement)

        add_executable(pulp-test-gpu-probe-native-recipes
            ${CMAKE_SOURCE_DIR}/test/test_gpu_probe_native_recipes.cpp)
        target_link_libraries(pulp-test-gpu-probe-native-recipes PRIVATE
            pulp::tool-gpu-probe-recipes Catch2::Catch2WithMain)
        add_test(NAME pulp-test-gpu-probe-native-recipes
            COMMAND pulp-test-gpu-probe-native-recipes)
        set_tests_properties(pulp-test-gpu-probe-native-recipes PROPERTIES
            LABELS "gpu;gpu-probe"
            RESOURCE_LOCK pulp_gpu)

    endif()
endif()
