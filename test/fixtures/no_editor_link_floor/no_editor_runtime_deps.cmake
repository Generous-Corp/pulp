# The dependency pattern that marks a module as carrying a GPU/window stack.
# Shared by scan_no_editor_link_floor.cmake and its self-test.
#
# A GPU runtime reaches a module as a shared WebGPU/Skia library, or, when Skia
# and Dawn are linked statically (a Skia build that carries its own Dawn links
# no wgpu-native), through what that static stack loads directly. On macOS that
# is the platform GPU API (Metal, MetalKit, QuartzCore). On Linux, Dawn opens
# Vulkan and GL with dlopen, so neither appears in NEEDED; the window-system
# client library the editor host links (X11, xcb, Wayland) does.
function(pulp_no_editor_runtime_deps out platform)
    set(_deps "libwgpu_native|libdawn|libwebgpu|libskia")
    if(platform STREQUAL "apple")
        string(APPEND _deps "|/Metal\\.framework/|/MetalKit\\.framework/|/QuartzCore\\.framework/")
    else()
        string(APPEND _deps "|libvulkan|libGL[.X]|libEGL|libX11\\.so|libxcb[.-]|libwayland-client")
    endif()
    set(${out} "${_deps}" PARENT_SCOPE)
endfunction()
