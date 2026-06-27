# SuperConvolver — convolution reverb / impulse processor.
# v1: RT-safe CPU convolution (signal::PartitionedConvolver) live engine;
# GPU convolution engine + GPU UI layered in next.

# Headless audio-validation test (golden: impulse -> IR, dry/bypass null).
if(PULP_BUILD_TESTS)
    add_executable(super-convolver-test test_super_convolver.cpp)
    target_link_libraries(super-convolver-test
        PRIVATE pulp::format pulp::signal Catch2::Catch2WithMain)
    target_include_directories(super-convolver-test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    catch_discover_tests(super-convolver-test)
endif()
# Plugin formats (VST3/AU/CLAP/standalone) + GPU UI are wired next (needs the
# entry glue + a create-fn registration); kept out until the audio is harness-
# validated.
