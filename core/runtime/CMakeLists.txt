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
    src/simd.cpp
)

# Highway's foreach_target.h uses #include HWY_TARGET_INCLUDE to re-include
# the source file for each SIMD target. The source dir must be on the include path.
target_include_directories(pulp-runtime PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)

target_link_libraries(pulp-runtime PUBLIC pulp::platform)
target_link_libraries(pulp-runtime PRIVATE hwy)

# CHOC headers (used by SpscQueue wrapper)
target_include_directories(pulp-runtime PUBLIC
    $<BUILD_INTERFACE:${choc_SOURCE_DIR}>
    $<INSTALL_INTERFACE:include>
)
