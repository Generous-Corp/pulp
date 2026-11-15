include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/g5_trigger_admission_tests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/g5_trigger_runtime_tests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/g5_control_index_tests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/g5_control_index_consumer_tests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/g5_host_event_consumer_tests.cmake")
# A2 composes the G-4 subgraph document with the G-5 Control clock.
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/a2_modular_instrument_consumer_tests.cmake")
