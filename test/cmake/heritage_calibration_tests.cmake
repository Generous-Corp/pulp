add_executable(pulp-heritage-cyclic-calibration-render
    sample_heritage_cyclic_calibration_render.cpp)
target_link_libraries(pulp-heritage-cyclic-calibration-render PRIVATE pulp::audio)

if(Python3_Interpreter_FOUND)
    add_test(NAME heritage-calibration-toolkit
        COMMAND ${Python3_EXECUTABLE} -m unittest test_heritage_calibration)
    set_tests_properties(heritage-calibration-toolkit PROPERTIES
        WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools/audio/heritage-calibration"
        ENVIRONMENT
            "PULP_HERITAGE_CYCLIC_RENDER_WAV=$<TARGET_FILE:pulp-heritage-cyclic-calibration-render>"
        LABELS "audio;sampler;heritage;quality-lab"
        TIMEOUT 60)
endif()
