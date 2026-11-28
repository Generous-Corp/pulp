# Pulp's audio widgets and display provider for Vellum: meters, scopes, the
# waveform editor, and the spectrogram and waveform displays. Optional; depends
# on Pulp runtime, signal and audio plus Vellum's value and view interfaces.
find_package(Vellum CONFIG REQUIRED)

add_library(pulp-vellum-audio STATIC src/audio_display_provider.cpp src/audio_widgets.cpp
    src/eq_curve_view.cpp src/graph_scale.cpp src/multi_meter.cpp src/musical_typing_keyboard.cpp src/piano_roll_pitch_ruler.cpp src/piano_roll_view.cpp
    src/musical_typing_keyboard_svg.cpp src/register_audio_widgets.cpp src/spectrogram_view.cpp
    src/standalone_typing_panel.cpp src/step_grid_view.cpp src/waveform_editor.cpp src/waveform_editor_primitives.cpp src/waveform_gpu_primitives.cpp src/waveform_gpu_render_controller.cpp
    src/waveform_headless_render_backend.cpp src/waveform_recorder_view.cpp
    src/waveform_recorder_view_svg.cpp src/waveform_view.cpp)
add_library(pulp::vellum-audio ALIAS pulp-vellum-audio)
target_include_directories(pulp-vellum-audio PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>)
target_compile_features(pulp-vellum-audio PUBLIC cxx_std_20)
target_link_libraries(pulp-vellum-audio PUBLIC pulp::runtime pulp::dataplane-audio pulp::signal pulp::audio pulp::format-core pulp::midi
    pulp::timeline pulp::timeline-editor pulp::timebase Vellum::Value Vellum::MirrorView_core)
if(TARGET pulp-vellum-scripted)
    target_link_libraries(pulp-vellum-scripted PUBLIC pulp::vellum-audio)
    target_compile_definitions(pulp-vellum-scripted PRIVATE PULP_VELLUM_AUDIO=1)
endif()

if(PULP_ENABLE_PULP_VELLUM)
    add_library(pulp-vellum-widget-gallery STATIC src/widget_gallery.cpp)
    add_library(pulp::vellum-widget-gallery ALIAS pulp-vellum-widget-gallery)
    target_link_libraries(pulp-vellum-widget-gallery PUBLIC pulp::vellum-audio)
endif()
