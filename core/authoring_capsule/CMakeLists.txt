add_library(pulp-authoring-capsule
    src/archive.cpp
    src/canonical_json.cpp
    src/canonical_pcm.cpp
    src/capsule.cpp
    src/completeness.cpp
    src/manifest.cpp
    src/profile_registry.cpp
    src/safe_path.cpp
    src/staging.cpp
    src/status.cpp)
add_library(pulp::authoring-capsule ALIAS pulp-authoring-capsule)

target_compile_features(pulp-authoring-capsule PUBLIC cxx_std_20)
target_include_directories(pulp-authoring-capsule
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)

# Deliberately narrow. The capsule substrate must stay reusable by a consumer
# that shares nothing with Forge, so it links the low-level runtime and the
# audio decode surface it needs for canonical PCM, and nothing above them. In
# particular it does not link pulp::project-package, which publicly depends on
# Timeline; the atomic-publication behaviour it needs is reimplemented here
# against the filesystem rather than borrowed through a Timeline dependency.
target_link_libraries(pulp-authoring-capsule
    PUBLIC pulp::runtime
    PRIVATE pulp::audio)

# miniz is vendored, not fetched, and pulp-runtime compiles it in while keeping
# its include directory PRIVATE — so no linked target publishes the header and
# this module has to name the vendored path itself, as tools/timeline does.
target_include_directories(pulp-authoring-capsule
    PRIVATE ${PROJECT_SOURCE_DIR}/external/miniz)

if(WIN32)
    target_link_libraries(pulp-authoring-capsule PRIVATE Advapi32)
endif()
