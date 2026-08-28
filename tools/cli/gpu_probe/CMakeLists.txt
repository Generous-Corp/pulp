add_library(pulp-tool-gpu-probe-model STATIC
    src/probe_result.cpp
    src/probe_result_json.cpp)
add_library(pulp::tool-gpu-probe-model ALIAS pulp-tool-gpu-probe-model)
target_compile_features(pulp-tool-gpu-probe-model PUBLIC cxx_std_20)
target_include_directories(pulp-tool-gpu-probe-model
    PUBLIC $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    PRIVATE $<BUILD_INTERFACE:${choc_SOURCE_DIR}>)

if(PULP_ENABLE_GPU)
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
        PRIVATE pulp::render pulp::runtime)
    if(PULP_ENABLE_SCENE3D)
        target_link_libraries(pulp-tool-gpu-probe-recipes PRIVATE pulp::scene)
    endif()

    if(PULP_ENABLE_SCENE3D)
        add_executable(pulp-gpu-probe-native-acceptance EXCLUDE_FROM_ALL
            src/native_acceptance.cpp)
        target_link_libraries(pulp-gpu-probe-native-acceptance PRIVATE
            pulp::tool-gpu-probe-recipes)
    endif()
endif()

if(PULP_BUILD_TESTS)
    add_executable(pulp-test-gpu-probe-result
        ${CMAKE_SOURCE_DIR}/test/test_gpu_probe_result.cpp)
    target_link_libraries(pulp-test-gpu-probe-result PRIVATE
        pulp::tool-gpu-probe-model Catch2::Catch2WithMain)
    add_test(NAME gpu-probe-result COMMAND pulp-test-gpu-probe-result)

    if(PULP_ENABLE_GPU)
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
