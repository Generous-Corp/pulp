# pulp-view-platform: the extracted D3 view-platform unit. It is the native
# (WebView excluded) composition of view-script, or of view-core when
# PULP_ENABLE_JS is OFF, plus the WebView backend probe, and it links no audio,
# MIDI or recognition-pack target. pulp-view-native is the Pulp-kept umbrella
# over it that adds the audio widgets and the built-in recognition pack, so its
# consumers keep the same surface. The per-OS backend probes and the
# design-import link are added where those targets are configured.
add_library(pulp-view-platform src/web_view_backend.cpp)
add_library(pulp::view-platform ALIAS pulp-view-platform)
target_compile_definitions(pulp-view-platform PUBLIC PULP_BUILD_WEBVIEW=0)
if(PULP_ENABLE_JS)
    target_link_libraries(pulp-view-platform PUBLIC pulp::view-script)
else()
    target_link_libraries(pulp-view-platform PUBLIC pulp::view-core)
endif()
