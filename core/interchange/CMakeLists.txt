add_library(pulp-interchange
    src/census.cpp
    src/export_plan.cpp
    src/loss.cpp
)
add_library(pulp::interchange ALIAS pulp-interchange)

target_compile_features(pulp-interchange PUBLIC cxx_std_20)

target_include_directories(pulp-interchange PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:include>)

# Format-neutral by construction: this target knows about documents and about
# what formats declare, never about any format's bytes. Readers and writers live
# in their own targets above it. The dependency floor is CI-enforced.
target_link_libraries(pulp-interchange
    PUBLIC
        pulp::timeline
        pulp::timebase
    PRIVATE
        pulp::runtime
)

if(CMAKE_CXX_COMPILER_ID MATCHES "Clang|GNU")
    target_compile_options(pulp-interchange PRIVATE -fno-exceptions -fno-rtti)
endif()
