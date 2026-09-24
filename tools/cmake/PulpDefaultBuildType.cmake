# Default an empty CMAKE_BUILD_TYPE to Release for single-config generators.
#
# A single-config generator with an empty CMAKE_BUILD_TYPE compiles with no
# optimization and no NDEBUG, which makes a JS-scripted GPU UI feel broken for
# reasons that have nothing to do with the code. Only the top-level project is
# defaulted, so a consumer that vendors Pulp keeps control of its own cache.
# Pass -DCMAKE_BUILD_TYPE=Debug (or PULP_BUILD_TYPE=Debug to the CLI) for a
# debuggable tree. Multi-config generators pick the config at build time.
get_property(_pulp_multi_config GLOBAL PROPERTY GENERATOR_IS_MULTI_CONFIG)
if(PROJECT_IS_TOP_LEVEL AND NOT _pulp_multi_config AND NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING
        "Build type (Debug, Release, RelWithDebInfo, MinSizeRel)" FORCE)
    message(STATUS "Pulp: CMAKE_BUILD_TYPE was empty; defaulting to Release")
endif()
