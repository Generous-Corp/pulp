cmake_minimum_required(VERSION 3.24)
project(PulpProcessorRestoreConsumer LANGUAGES CXX)

find_package(Pulp CONFIG REQUIRED COMPONENTS host)
add_executable(pulp-processor-restore-consumer main.cpp)
target_compile_features(pulp-processor-restore-consumer PRIVATE cxx_std_20)
target_link_libraries(pulp-processor-restore-consumer PRIVATE Pulp::host)

enable_testing()
add_test(NAME processor-restore-consumer COMMAND pulp-processor-restore-consumer)
# The proof moves 32 MiB aggregate state through serialize/restore and compares
# it byte by byte, so a sanitizer's per-access instrumentation dominates. On
# arm64: about 5 s in Release, about 51 s Debug + AddressSanitizer (the
# sanitizer lane's configuration), which a flat 30 s limit cannot hold. Keep the
# tight limit everywhere else so a real hang is still caught quickly; PULP_SANITIZER
# is forwarded by the host test (test_g2_processor_restore_sdk_consumer.cmake).
set(_restore_timeout 30)
if(PULP_SANITIZER)
    set(_restore_timeout 180)
endif()
set_tests_properties(processor-restore-consumer PROPERTIES
    LABELS "examples;installed-sdk;host" TIMEOUT ${_restore_timeout})
