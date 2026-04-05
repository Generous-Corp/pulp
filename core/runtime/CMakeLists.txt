add_library(pulp-runtime)
add_library(pulp::runtime ALIAS pulp-runtime)

target_include_directories(pulp-runtime
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-runtime PRIVATE
    src/runtime.cpp
    src/identity.cpp
)

target_link_libraries(pulp-runtime PUBLIC pulp::platform)

# CHOC headers (used by SpscQueue wrapper)
target_include_directories(pulp-runtime PUBLIC
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)
