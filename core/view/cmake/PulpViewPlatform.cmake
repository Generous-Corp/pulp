# pulp-view-platform: the extracted D3 view-platform unit. It is the native
# (WebView excluded) composition of view-script, or of view-core when
# PULP_ENABLE_JS is OFF, plus the one WebView backend detector, and it links no
# audio, MIDI or recognition-pack target. The detector reports "none" until a
# composition that compiles WebView in installs the per-OS provider
# (pulp-view-webview-backend). pulp-view-native is the Pulp-kept umbrella over
# it that adds the audio widgets and the built-in recognition pack, so its
# consumers keep the same surface. The design-import link is added where that
# target is configured.
add_library(pulp-view-platform src/web_view_backend.cpp)
add_library(pulp::view-platform ALIAS pulp-view-platform)
if(PULP_ENABLE_JS)
    target_link_libraries(pulp-view-platform PUBLIC pulp::view-script)
else()
    target_link_libraries(pulp-view-platform PUBLIC pulp::view-core)
endif()
