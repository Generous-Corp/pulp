# brew-rig — the loopback doctor. The only thing in the suite that opens real
# hardware, and the only way to close the hardware-verification debt.

if(PULP_BUILD_TESTS)
    add_executable(brew-rig-test test_rig.cpp)
    target_link_libraries(brew-rig-test PRIVATE brew-core Catch2::Catch2WithMain)
    catch_discover_tests(brew-rig-test)
endif()

# Needs a real audio backend, so it only builds where pulp::audio has one.
add_executable(brew-rig brew_rig.cpp)
target_link_libraries(brew-rig PRIVATE brew-core pulp::audio)
