# Stream feature demo — async HTTP fetch + file I/O via unified Stream API.
add_executable(pulp-stream-demo main.cpp)
target_link_libraries(pulp-stream-demo PRIVATE pulp::runtime pulp::events)
