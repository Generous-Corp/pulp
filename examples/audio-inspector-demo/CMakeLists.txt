# Audio Inspector demo: run_with_editor plus continuous output audio.

add_executable(pulp-audio-inspector-demo main.cpp)
target_link_libraries(pulp-audio-inspector-demo PRIVATE pulp::standalone)

add_executable(pulp-audio-inspector-demo-render render_proof_wav.cpp)
target_link_libraries(pulp-audio-inspector-demo-render PRIVATE pulp::format)
