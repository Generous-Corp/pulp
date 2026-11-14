# pulp-texture-upload-recorder: records every texture upload in this process so
# a test or harness can prove two editors upload byte-identical textures.
#
# Test-only tooling: link it into test executables and test hosts only, never
# into a shipped plug-in or an SDK runtime library. It is not installed or
# exported, and records nothing until install() is called.
if(NOT APPLE OR IOS)
    return()
endif()

add_library(pulp-texture-upload-recorder STATIC
    src/recorder_core.cpp
    src/recorder_metal.mm)
add_library(pulp::texture-upload-recorder ALIAS pulp-texture-upload-recorder)
target_include_directories(pulp-texture-upload-recorder
    PUBLIC ${CMAKE_CURRENT_SOURCE_DIR}/include
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
target_compile_features(pulp-texture-upload-recorder PUBLIC cxx_std_20)
# The Metal hooks hand a +1 result straight back from a `new...` method, which
# needs manual reference counting (the file refuses to build under ARC).
set_source_files_properties(src/recorder_metal.mm PROPERTIES COMPILE_OPTIONS -fno-objc-arc)
target_link_libraries(pulp-texture-upload-recorder
    PRIVATE pulp::runtime "-framework Metal" "-framework Foundation")
