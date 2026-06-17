# Component shots — headless per-component renderer for the Figma-vs-Pulp
# comparison contact sheet. Desktop only (CoreGraphics provider on Apple).
if(NOT PULP_IOS AND NOT ANDROID)
    add_executable(pulp-component-shots main.cpp)
    target_link_libraries(pulp-component-shots PRIVATE pulp::view)
endif()
