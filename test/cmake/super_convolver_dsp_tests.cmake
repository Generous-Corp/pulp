# SuperConvolver DSP regression tests (offline/bounce fidelity, Size headroom,
# Flow audibility). The processor lives in examples/super-convolver, so this
# links the example's view TU for the processor vtable (create_view()) and adds
# the example dir to the include path — mirroring examples/super-convolver's own
# super-convolver-test target. GPU-path assertions skip cleanly with no device.
if(PULP_BUILD_TESTS AND TARGET pulp::gpu-audio AND TARGET pulp::render)
    pulp_add_test_suite(pulp-test-super-convolver-dsp
        SOURCES test_super_convolver_dsp.cpp
                ${CMAKE_SOURCE_DIR}/examples/super-convolver/super_convolver_view.cpp
        LIBRARIES pulp::format pulp::signal pulp::gpu-audio pulp::render
                  pulp::view pulp::canvas
        INCLUDE_DIRS ${CMAKE_SOURCE_DIR}/examples/super-convolver)
endif()
