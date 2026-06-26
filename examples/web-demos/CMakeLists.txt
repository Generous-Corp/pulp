# Web demo plugins — designed for WASM compilation and browser hosting
# These plugins demonstrate Pulp's cross-platform story:
#   Native: cmake --build build --target web-demos
#   WASM:   emcmake cmake ... && cmake --build ... --target web-demos
#   WCLAP:  cmake -DCMAKE_TOOLCHAIN_FILE=tools/cmake/wasi-toolchain.cmake ...

add_library(pulp-web-demos STATIC
    web_demos.cpp
)

target_link_libraries(pulp-web-demos PUBLIC pulp::format)
target_include_directories(pulp-web-demos PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${CMAKE_CURRENT_SOURCE_DIR}/../pulp-pluck
)
