add_library(pulp-music INTERFACE)
add_library(pulp::music ALIAS pulp-music)

target_compile_features(pulp-music INTERFACE cxx_std_20)

target_include_directories(pulp-music
    INTERFACE
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)
