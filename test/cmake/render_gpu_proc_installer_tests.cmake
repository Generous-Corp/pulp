# Dawn proc-table installer seam test registrations.
# Included by render_gpu_tests.cmake under PULP_ENABLE_GPU.

    # The installer policy itself, on fresh registry instances. It reaches into
    # core/render/src/ for the internal registry header and needs no GPU.
    add_executable(pulp-test-gpu-proc-installer test_gpu_proc_installer.cpp)
    target_link_libraries(pulp-test-gpu-proc-installer
        PRIVATE pulp::gpu-device-api Catch2::Catch2WithMain)
    target_include_directories(pulp-test-gpu-proc-installer PRIVATE
        ${CMAKE_SOURCE_DIR}/core/render/src)
    catch_discover_tests(pulp-test-gpu-proc-installer)

    # The process-global policy through the real GpuSurface and GpuCompute. One
    # test case per process: the first GPU object seals the policy.
    pulp_add_test_suite(pulp-test-gpu-proc-installer-host-policy
        SOURCES test_gpu_proc_installer_host_policy.cpp
        LIBRARIES pulp::render
        DISCOVERY_ARGS ${PULP_GPU_TEST_DISCOVERY_ARGS})

    # The PULP_DAWN_PROCS_HOST_OWNED setting, without reconfiguring the build:
    # a test-only copy of pulp-gpu-device compiled from the same sources with
    # that one definition added. In a build already configured ON, the real
    # library is the host-owned one and the copy is not needed.
    if(TARGET pulp-gpu-device AND NOT PULP_DAWN_PROCS_HOST_OWNED)
        get_target_property(_pulp_dawn_procs_sources pulp-gpu-device SOURCES)
        get_target_property(_pulp_dawn_procs_dir pulp-gpu-device SOURCE_DIR)
        set(_pulp_dawn_procs_abs_sources "")
        foreach(_pulp_dawn_procs_src IN LISTS _pulp_dawn_procs_sources)
            if(IS_ABSOLUTE "${_pulp_dawn_procs_src}")
                list(APPEND _pulp_dawn_procs_abs_sources "${_pulp_dawn_procs_src}")
            else()
                list(APPEND _pulp_dawn_procs_abs_sources
                    "${_pulp_dawn_procs_dir}/${_pulp_dawn_procs_src}")
            endif()
        endforeach()
        add_library(pulp-test-gpu-device-host-owned STATIC ${_pulp_dawn_procs_abs_sources})
        foreach(_pulp_dawn_procs_prop IN ITEMS
                LINK_LIBRARIES INCLUDE_DIRECTORIES COMPILE_DEFINITIONS COMPILE_OPTIONS
                COMPILE_FEATURES)
            get_target_property(_pulp_dawn_procs_value pulp-gpu-device ${_pulp_dawn_procs_prop})
            if(_pulp_dawn_procs_value)
                set_property(TARGET pulp-test-gpu-device-host-owned
                    PROPERTY ${_pulp_dawn_procs_prop} "${_pulp_dawn_procs_value}")
            endif()
        endforeach()
        get_target_property(_pulp_dawn_procs_value pulp-gpu-device INTERFACE_LINK_LIBRARIES)
        if(_pulp_dawn_procs_value)
            set_property(TARGET pulp-test-gpu-device-host-owned
                PROPERTY INTERFACE_LINK_LIBRARIES "${_pulp_dawn_procs_value}")
        endif()
        target_compile_definitions(pulp-test-gpu-device-host-owned
            PRIVATE PULP_DAWN_PROCS_HOST_OWNED=1)

        foreach(_pulp_dawn_procs_case IN ITEMS host-owned host-owned-registered)
            string(REPLACE "-" "_" _pulp_dawn_procs_file "${_pulp_dawn_procs_case}")
            add_executable(pulp-test-gpu-proc-installer-${_pulp_dawn_procs_case}
                test_gpu_proc_installer_${_pulp_dawn_procs_file}.cpp)
            target_link_libraries(pulp-test-gpu-proc-installer-${_pulp_dawn_procs_case}
                PRIVATE pulp-test-gpu-device-host-owned Catch2::Catch2WithMain)
            catch_discover_tests(pulp-test-gpu-proc-installer-${_pulp_dawn_procs_case}
                ${PULP_GPU_TEST_DISCOVERY_ARGS})
        endforeach()

        # The host-owned copy references no proc-table setter at all, so no
        # Pulp path can install Dawn's native procs; the default library is
        # the control that proves the scan sees the reference.
        if(Python3_Interpreter_FOUND AND NOT WIN32)
            add_test(NAME dawn-proc-setter-absent-when-host-owned
                COMMAND ${Python3_EXECUTABLE}
                        ${CMAKE_SOURCE_DIR}/test/verify_dawn_proc_setter_absent.py
                        $<TARGET_FILE:pulp-test-gpu-device-host-owned>
                        $<TARGET_FILE:pulp-gpu-device>)
            set_tests_properties(dawn-proc-setter-absent-when-host-owned PROPERTIES
                LABELS "render;gpu-device" TIMEOUT 60 SKIP_RETURN_CODE 77)
        endif()
        unset(_pulp_dawn_procs_sources)
        unset(_pulp_dawn_procs_dir)
        unset(_pulp_dawn_procs_abs_sources)
        unset(_pulp_dawn_procs_src)
        unset(_pulp_dawn_procs_prop)
        unset(_pulp_dawn_procs_value)
        unset(_pulp_dawn_procs_case)
        unset(_pulp_dawn_procs_file)
    endif()
