# Musical Typing Keyboard — focused live demo (GPU window). Desktop only; uses
# the GPU host (MacGpuWindowHost) when built with Skia, for the faithful SVG.
if(NOT PULP_IOS AND NOT ANDROID)
    add_executable(pulp-mtk-demo main.cpp)
    target_link_libraries(pulp-mtk-demo PRIVATE pulp::view)
endif()
