# pulp-midi-ports: lists MIDI sources and destinations with their Pulp ids.
add_executable(pulp-midi-ports midi_ports.cpp)
target_compile_features(pulp-midi-ports PRIVATE cxx_std_20)
target_link_libraries(pulp-midi-ports PRIVATE pulp::midi)
