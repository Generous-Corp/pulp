# pulp-rt-allocation-probe: the RT allocation probe for SDK consumers' tests.
#
# A static library holding pulp::test::RtAllocationProbe and the replacement
# global allocation operators it counts through. Exported as
# Pulp::rt-allocation-probe; link it into test executables only, since it
# replaces operator new and delete for the whole executable. In-tree tests
# compile the same source through test/harness/rt_allocation_probe.cpp.
add_library(pulp-rt-allocation-probe STATIC src/rt_allocation_probe.cpp)
add_library(pulp::rt-allocation-probe ALIAS pulp-rt-allocation-probe)
target_include_directories(pulp-rt-allocation-probe
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>)
target_compile_features(pulp-rt-allocation-probe PUBLIC cxx_std_20)
