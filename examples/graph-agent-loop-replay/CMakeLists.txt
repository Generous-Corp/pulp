cmake_minimum_required(VERSION 3.24)
project(PulpGraphAgentLoopReplay LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)

add_executable(pulp-graph-agent-loop-replay main.cpp)
target_compile_features(pulp-graph-agent-loop-replay PRIVATE cxx_std_20)
target_link_libraries(pulp-graph-agent-loop-replay PRIVATE Pulp::host)
target_compile_definitions(pulp-graph-agent-loop-replay PRIVATE
    A9_REPLAY_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/fixtures")

enable_testing()
add_test(NAME graph-agent-loop-replay-positive
         COMMAND pulp-graph-agent-loop-replay
                 "${CMAKE_CURRENT_BINARY_DIR}/positive"
                 "${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay-plan.json")

foreach(mutation IN ITEMS wrong-edit wrong-replay)
    add_test(NAME graph-agent-loop-replay-${mutation}
             COMMAND ${CMAKE_COMMAND}
                     -DGRAPH_AGENT_LOOP_REPLAY_EXECUTABLE=$<TARGET_FILE:pulp-graph-agent-loop-replay>
                     -DGRAPH_AGENT_LOOP_REPLAY_OUTPUT=${CMAKE_CURRENT_BINARY_DIR}/${mutation}
                     -DGRAPH_AGENT_LOOP_REPLAY_PLAN=${CMAKE_CURRENT_SOURCE_DIR}/fixtures/replay-plan-${mutation}.json
                     -DGRAPH_AGENT_LOOP_REPLAY_RULE=${mutation}-
                     -P ${CMAKE_CURRENT_SOURCE_DIR}/assert-failure.cmake)
endforeach()
