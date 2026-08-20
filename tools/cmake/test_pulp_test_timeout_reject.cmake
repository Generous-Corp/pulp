# Helper for test_pulp_test_timeout.cmake: a scale below 1 must FATAL_ERROR.
# Split into its own script because a FATAL_ERROR cannot be caught in-process.
cmake_minimum_required(VERSION 3.20)
include(${CMAKE_CURRENT_LIST_DIR}/PulpTestTimeout.cmake)
pulp_scaled_test_timeout(_out 100)
