# Audio Inspector demo: run_with_editor plus continuous output audio.

add_executable(pulp-audio-inspector-demo main.cpp)
target_link_libraries(pulp-audio-inspector-demo PRIVATE pulp::standalone)

add_executable(pulp-audio-inspector-demo-render render_proof_wav.cpp)
target_link_libraries(pulp-audio-inspector-demo-render PRIVATE pulp::format)

if(PULP_BUILD_TESTS)
    add_test(
        NAME audio-inspector-demo-render-proof
        COMMAND ${CMAKE_COMMAND}
            -DRENDER_EXE=$<TARGET_FILE:pulp-audio-inspector-demo-render>
            -DOUTPUT_DIR=${CMAKE_CURRENT_BINARY_DIR}/render-proof-test
            -P ${CMAKE_CURRENT_SOURCE_DIR}/render_proof_test.cmake
    )
    set_tests_properties(audio-inspector-demo-render-proof PROPERTIES
        LABELS "examples;audio-inspector"
        TIMEOUT 15)
endif()
