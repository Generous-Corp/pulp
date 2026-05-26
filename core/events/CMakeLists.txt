add_library(pulp-events)
add_library(pulp::events ALIAS pulp-events)

target_include_directories(pulp-events
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_sources(pulp-events PRIVATE
    src/event_loop.cpp
    src/main_thread_dispatcher.cpp
    src/timer.cpp
    src/async_updater.cpp
    src/interprocess_connection.cpp
    src/child_process_manager.cpp
    src/volume_detector.cpp
    src/service_discovery.cpp
)

# Item 6.4b — plugin-mode main-thread cooperation. macOS gets a Cocoa
# (`dispatch_get_main_queue`) backend so plugin adapters can opt the
# process into MainThreadDispatcher without owning a Pulp WindowHost.
# Other platforms get a no-op stub; their own message-loop integration
# lands later (gap-doc Phase 1).
if(APPLE)
    target_sources(pulp-events PRIVATE src/plugin_main_thread_mac.mm)
else()
    target_sources(pulp-events PRIVATE src/plugin_main_thread_stub.cpp)
endif()

# Phase 2 (gap-doc 2026-05-24): platform mDNS backends for
# NetworkServiceDiscovery. macOS / iOS get a real Bonjour backend
# linked against the system DNS-SD APIs (exported from CoreServices
# on macOS, libsystem on iOS). Linux + Windows resolve their stacks
# at run-time via `pulp::runtime::DynamicLibrary` so the build never
# hard-fails on machines that don't have avahi-daemon / the Bonjour
# SDK installed; `install_default_backend()` reports false when the
# runtime library isn't there so callers can degrade explicitly.
if(APPLE)
    target_sources(pulp-events PRIVATE
        platform/mac/bonjour_backend.cpp
    )
    if(NOT IOS)
        target_link_libraries(pulp-events PRIVATE
            "-framework CoreServices"
        )
    endif()
elseif(UNIX AND NOT APPLE)
    # Linux Avahi backend — runtime-dlopen libavahi-client.so.3; no
    # build-time dependency on the Avahi headers or libs.
    target_sources(pulp-events PRIVATE
        platform/linux/avahi_backend.cpp
    )
endif()

target_link_libraries(pulp-events PUBLIC pulp::runtime)
