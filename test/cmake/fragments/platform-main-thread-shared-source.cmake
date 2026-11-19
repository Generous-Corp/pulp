# MainThreadScope's shared run-loop source: thousands of scopes add one source,
# order and destruction guarantees hold, posting races destruction.
if(APPLE AND NOT PULP_IOS)
    pulp_add_test_suite(pulp-test-platform-main-thread-shared-source
        SOURCES test_platform_main_thread_shared_source.mm
        LIBRARIES pulp::platform)
    target_link_libraries(pulp-test-platform-main-thread-shared-source PRIVATE
        "-framework CoreFoundation")
endif()
