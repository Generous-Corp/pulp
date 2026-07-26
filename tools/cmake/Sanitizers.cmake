# Sanitizers.cmake — Add sanitizer support to Pulp builds
#
# Usage:
#   cmake -B build -DPULP_SANITIZER=address   # AddressSanitizer
#   cmake -B build -DPULP_SANITIZER=thread    # ThreadSanitizer
#   cmake -B build -DPULP_SANITIZER=undefined # UndefinedBehaviorSanitizer
#   cmake -B build -DPULP_SANITIZER=memory    # MemorySanitizer (Clang only)
#   cmake -B build -DPULP_SANITIZER=realtime  # RealtimeSanitizer (Clang 18+)

set(PULP_SANITIZER "" CACHE STRING "Enable sanitizer: address, thread, undefined, memory, realtime")
set(PULP_SANITIZER_COMPILE_FLAGS)
set(PULP_SANITIZER_LINK_FLAGS)

if(PULP_SANITIZER)
    message(STATUS "Pulp: Sanitizer enabled: ${PULP_SANITIZER}")

    if(PULP_SANITIZER STREQUAL "address")
        set(PULP_SANITIZER_COMPILE_FLAGS
            -fsanitize=address -fno-omit-frame-pointer)
        set(PULP_SANITIZER_LINK_FLAGS -fsanitize=address)
    elseif(PULP_SANITIZER STREQUAL "thread")
        set(PULP_SANITIZER_COMPILE_FLAGS -fsanitize=thread)
        set(PULP_SANITIZER_LINK_FLAGS -fsanitize=thread)
    elseif(PULP_SANITIZER STREQUAL "undefined")
        set(PULP_SANITIZER_COMPILE_FLAGS
            -fsanitize=undefined -fno-sanitize-recover=all)
        set(PULP_SANITIZER_LINK_FLAGS -fsanitize=undefined)
    elseif(PULP_SANITIZER STREQUAL "memory")
        set(PULP_SANITIZER_COMPILE_FLAGS
            -fsanitize=memory -fno-omit-frame-pointer)
        set(PULP_SANITIZER_LINK_FLAGS -fsanitize=memory)
    elseif(PULP_SANITIZER STREQUAL "realtime")
        # RealtimeSanitizer (RTSan) — detects real-time safety violations
        # such as memory allocation, mutex locks, or syscalls in audio callbacks.
        # Requires: Clang 18+ (LLVM) — not available in Apple Clang as of Xcode 16.
        # See: https://clang.llvm.org/docs/RealtimeSanitizer.html
        #
        # Platform support:
        #   Linux (x86_64, aarch64): Fully supported with Clang 18+
        #   macOS: Requires upstream LLVM Clang 18+, NOT Apple Clang
        #   Windows: Not supported
        set(PULP_SANITIZER_COMPILE_FLAGS
            -fsanitize=realtime -fno-omit-frame-pointer)
        set(PULP_SANITIZER_LINK_FLAGS -fsanitize=realtime)
    else()
        message(WARNING "Unknown sanitizer: ${PULP_SANITIZER}")
    endif()

    # Force debug info for meaningful stack traces
    if(NOT CMAKE_BUILD_TYPE STREQUAL "Debug")
        list(APPEND PULP_SANITIZER_COMPILE_FLAGS -g)
    endif()

    add_compile_options(${PULP_SANITIZER_COMPILE_FLAGS})
    add_link_options(${PULP_SANITIZER_LINK_FLAGS})
endif()
