# A plug-in-author-shaped project built against an INSTALLED Pulp SDK. It emits
# trace spans through the public macros and starts and stops the session itself,
# so tools/scripts/trace_consumer_e2e.sh can prove that a span written by
# consumer code reaches a captured .pftrace, and that an untraced SDK yields
# none. It deliberately links nothing but Pulp::runtime: the claim under test is
# what an SDK consumer gets, not what the source tree gets.
cmake_minimum_required(VERSION 3.24)
project(PulpTraceConsumer LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

find_package(Pulp CONFIG REQUIRED)

# EXPECT_TRACING comes from the caller, never from the package, so running the
# project against both SDK flavors proves the capability check can fail.
if(NOT DEFINED EXPECT_TRACING)
    message(FATAL_ERROR "EXPECT_TRACING must be set by the caller")
endif()
if(EXPECT_TRACING AND NOT PULP_HAS_TRACING)
    message(FATAL_ERROR "installed SDK does not advertise tracing")
endif()
if(NOT EXPECT_TRACING AND PULP_HAS_TRACING)
    message(FATAL_ERROR "installed SDK unexpectedly advertises tracing")
endif()

add_executable(trace-consumer main.cpp)
target_link_libraries(trace-consumer PRIVATE Pulp::runtime)
