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

# Phase 2 (gap-doc 2026-05-24): platform mDNS backends for
# NetworkServiceDiscovery. macOS / iOS get a real Bonjour backend
# linked against the system DNS-SD APIs (exported from CoreServices
# on macOS, libsystem on iOS). Linux (Avahi) and Windows (Bonjour
# SDK / WinRT NsdManager) backends are deferred to a follow-up PR;
# install_default_backend() returns false on those platforms today.
if(APPLE)
    target_sources(pulp-events PRIVATE
        platform/mac/bonjour_backend.cpp
    )
    if(NOT IOS)
        target_link_libraries(pulp-events PRIVATE
            "-framework CoreServices"
        )
    endif()
endif()

target_link_libraries(pulp-events PUBLIC pulp::runtime)
