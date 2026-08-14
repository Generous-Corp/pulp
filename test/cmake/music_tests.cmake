pulp_add_test_suite(pulp-test-music-theory LIBRARIES pulp::music)
pulp_add_test_suite(pulp-test-music-generative LIBRARIES pulp::music)
pulp_add_test_suite(pulp-test-music-pattern-development
    LIBRARIES pulp::music
    SOURCES test_music_pattern_development.cpp harness/rt_allocation_probe.cpp
    INCLUDE_DIRS ${CMAKE_CURRENT_SOURCE_DIR})
add_library(pulp-test-music-pattern-development-noexceptions OBJECT
    test_music_pattern_development_noexceptions.cpp)
target_link_libraries(pulp-test-music-pattern-development-noexceptions PRIVATE pulp::music)
add_dependencies(pulp-test-music-pattern-development
    pulp-test-music-pattern-development-noexceptions)
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-test-music-pattern-development-noexceptions PRIVATE
        -fno-exceptions -fno-rtti)
endif()
pulp_add_test_suite(pulp-test-music-voicing LIBRARIES pulp::music)
pulp_add_test_suite(pulp-test-music-compatibility
    LIBRARIES pulp::music pulp::signal pulp::timeline
    INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/core/timeline/src)
