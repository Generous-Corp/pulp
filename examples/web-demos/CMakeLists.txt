# Web demo plugins — designed for WASM compilation and browser hosting
# These plugins demonstrate Pulp's cross-platform story:
#   Native: cmake --build build --target web-demos
#   WASM:   emcmake cmake ... && cmake --build ... --target web-demos
#   WCLAP:  cmake -DCMAKE_TOOLCHAIN_FILE=tools/cmake/wasi-toolchain.cmake ...

add_library(pulp-web-demos STATIC
    web_demos.cpp
)

# The browser GPU-compute worker module. Emscripten-only: under the native
# toolchain the subdirectory declares no target (see its guard), so this costs
# the native build nothing.
add_subdirectory(gpu-audio)

target_link_libraries(pulp-web-demos PUBLIC pulp::format)
target_include_directories(pulp-web-demos PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../pulp-pluck
)
