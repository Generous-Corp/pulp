# OfflineStretch — offline time-stretch / pitch engine dev harness.
#
# stretchcli drives pulp::signal::OfflineStretch from the command line
# (file in -> render -> file out, plus --analyze). It is the development +
# corpus-rendering tool for the engine; see
# planning/Sampler-Offline-Stretch-Build-Plan.md.

add_executable(stretchcli stretchcli.cpp)
target_link_libraries(stretchcli PRIVATE pulp::audio pulp::signal)
target_include_directories(stretchcli PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
