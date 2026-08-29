if(TARGET pulp-test-gpu-health-result
        AND TARGET pulp-test-gpu-health-read-result)
    add_test(NAME gpu-health-result COMMAND pulp-test-gpu-health-result)
    set_tests_properties(gpu-health-result PROPERTIES
        LABELS "gpu;gpu-health;contract")

    add_test(NAME gpu-health-read-result COMMAND pulp-test-gpu-health-read-result)
    set_tests_properties(gpu-health-read-result PROPERTIES
        LABELS "gpu;gpu-health;read;contract")
endif()

if(TARGET pulp-test-gpu-health-provider)
    add_test(NAME gpu-health-provider COMMAND pulp-test-gpu-health-provider)
    set_tests_properties(gpu-health-provider PROPERTIES
        LABELS "gpu;gpu-health")
    if(PULP_ENABLE_SCENE3D)
        set_tests_properties(gpu-health-provider PROPERTIES
            LABELS "gpu;gpu-health;scene3d-required")
    endif()
endif()

add_executable(pulp-test-control-gpu-health-read-executor
    test_control_gpu_health_read_executor.cpp)
target_link_libraries(pulp-test-control-gpu-health-read-executor PRIVATE
    pulp::inspect-runtime pulp::tool-gpu-health-model Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-gpu-health-read-executor
    PROPERTIES LABELS "inspect;control;gpu;read")

add_executable(pulp-test-control-gpu-health-provider
    test_control_gpu_health_provider.cpp)
target_link_libraries(pulp-test-control-gpu-health-provider PRIVATE
    pulp::inspect-ui-runtime pulp::tool-gpu-health-model Catch2::Catch2WithMain)
catch_discover_tests(pulp-test-control-gpu-health-provider
    PROPERTIES LABELS "inspect;control;gpu;provider")

if(APPLE AND NOT IOS AND NOT PULP_IOS AND PULP_ENABLE_GPU AND
        TARGET pulp-inspect-standalone-runtime)
    add_executable(pulp-control-gpu-health-standalone-product-fixture
        fixtures/control_gpu_health_standalone_product_fixture.cpp)
    target_link_libraries(pulp-control-gpu-health-standalone-product-fixture PRIVATE
        pulp::inspect-standalone-runtime pulp::standalone)
    target_link_options(pulp-control-gpu-health-standalone-product-fixture PRIVATE
        "LINKER:-dead_strip_dylibs")
    _pulp_cache_control_declarations(
        pulp-control-gpu-health-standalone-product-fixture
        developer-local
        "dev.pulp.gpu/health.read@1" FALSE)
    _pulp_configure_control_shipping(
        pulp-control-gpu-health-standalone-product-fixture
        "dev.pulp.test.gpu-health-standalone-product"
        "Pulp GPU Health Standalone Product Fixture")
    _pulp_attach_control_shipping(
        pulp-control-gpu-health-standalone-product-fixture
        pulp-control-gpu-health-standalone-product-fixture Standalone)
    if(_pulp_control_host_codesign)
        add_custom_command(
            TARGET pulp-control-gpu-health-standalone-product-fixture POST_BUILD
            COMMAND "${_pulp_control_host_codesign}" --force --sign - --options library
                    "$<TARGET_FILE:pulp-control-gpu-health-standalone-product-fixture>"
            COMMAND /bin/chmod 0700
                    "$<TARGET_FILE:pulp-control-gpu-health-standalone-product-fixture>"
            COMMAND /bin/chmod 0600
                    "$<TARGET_FILE:pulp-control-gpu-health-standalone-product-fixture>.inspector-capabilities.json"
            COMMENT "Ad-hoc signing GPU health Standalone product fixture"
            VERBATIM)
    endif()

    add_executable(pulp-test-control-gpu-health-standalone-product
        test_control_gpu_health_standalone_product.cpp
        ${CMAKE_SOURCE_DIR}/inspect/src/control_broker_daemon.cpp)
    set_target_properties(pulp-test-control-gpu-health-standalone-product PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_BINARY_DIR}/a3-product")
    target_include_directories(pulp-test-control-gpu-health-standalone-product PRIVATE
        ${CMAKE_SOURCE_DIR}/inspect/src
        ${CMAKE_SOURCE_DIR}/test)
    target_link_libraries(pulp-test-control-gpu-health-standalone-product PRIVATE
        pulp::inspect-client Catch2::Catch2WithMain)
    target_compile_definitions(pulp-test-control-gpu-health-standalone-product PRIVATE
        PULP_CONTROL_GPU_HEALTH_STANDALONE_PRODUCT_FIXTURE="$<TARGET_FILE:pulp-control-gpu-health-standalone-product-fixture>"
        $<$<BOOL:${PULP_SANITIZER}>:PULP_TEST_WITH_SANITIZER=1>)
    add_dependencies(pulp-test-control-gpu-health-standalone-product
        pulp-control-gpu-health-standalone-product-fixture)
    if(_pulp_control_host_codesign)
        add_custom_command(
            TARGET pulp-test-control-gpu-health-standalone-product POST_BUILD
            COMMAND "${_pulp_control_host_codesign}" --force --sign -
                    "$<TARGET_FILE:pulp-test-control-gpu-health-standalone-product>"
            COMMAND "${CMAKE_COMMAND}" -E copy
                    "$<TARGET_FILE:pulp-test-control-gpu-health-standalone-product>"
                    "$<TARGET_FILE_DIR:pulp-test-control-gpu-health-standalone-product>/pulp"
            COMMENT "Ad-hoc signing GPU health Standalone product test"
            VERBATIM)
    endif()
    catch_discover_tests(pulp-test-control-gpu-health-standalone-product
        PROPERTIES LABELS "inspect;control;gpu;standalone;product")
endif()
