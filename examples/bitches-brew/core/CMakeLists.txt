# brew-core — the pieces every Bitches Brew plug-in shares: the CV output
# convention, the position-derived clock grid, the pulse-width rules, and the
# run-segment origin. Header-only; each plug-in composes it into one Processor.

add_library(brew-core INTERFACE)
target_include_directories(brew-core INTERFACE ${CMAKE_CURRENT_SOURCE_DIR}/include)

if(PULP_BUILD_TESTS)
    add_executable(brew-core-test test_brew_core.cpp)
    target_link_libraries(brew-core-test PRIVATE brew-core Catch2::Catch2WithMain)
    catch_discover_tests(brew-core-test)
endif()
