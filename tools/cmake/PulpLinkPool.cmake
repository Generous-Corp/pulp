# Cap concurrent link steps under Ninja.
#
# Every Pulp executable and plug-in bundle statically links the same large
# Skia/Dawn/SDL3 archives, so a wide build that reaches its link phase runs
# many links at once, each streaming ~150 MiB of archives. Those links contend
# for memory bandwidth and I/O rather than CPU: each one gets slower and peak
# RSS climbs with the number in flight. A Ninja job pool bounds the links
# without lowering the compile parallelism the governor granted.
#
# PULP_LINK_JOBS: empty derives the depth from physical RAM, 0 disables the
# pool, and a positive number is used as-is. Makefile, Xcode, and Visual Studio
# generators have no job pools, so this is a no-op there.

set(PULP_LINK_JOBS "" CACHE STRING
    "Concurrent link steps under Ninja (empty: derive from RAM, 0: unpooled)")

# One link slot per 2 GiB of physical RAM, clamped to [2, 8]. A test-program
# link peaks near 0.45 GiB RSS, so 2 GiB a slot leaves room for the compiles
# running beside it; an 8 GiB gate VM gets 4 slots, the same as its governed
# -j, so the pool never throttles a build that memory can already hold. The
# upper clamp bounds the memory-bandwidth contention a wide link phase causes
# on a large host.
set(_PULP_LINK_GIB_PER_JOB 2)
set(_PULP_LINK_MIN_JOBS 2)
set(_PULP_LINK_MAX_JOBS 8)

function(pulp_link_pool_default_depth ram_mib out_var)
    math(EXPR _jobs "${ram_mib} / (${_PULP_LINK_GIB_PER_JOB} * 1024)")
    if(_jobs LESS ${_PULP_LINK_MIN_JOBS})
        set(_jobs ${_PULP_LINK_MIN_JOBS})
    elseif(_jobs GREATER ${_PULP_LINK_MAX_JOBS})
        set(_jobs ${_PULP_LINK_MAX_JOBS})
    endif()
    set(${out_var} ${_jobs} PARENT_SCOPE)
endfunction()

if(CMAKE_GENERATOR MATCHES "Ninja")
    if(PULP_LINK_JOBS STREQUAL "")
        cmake_host_system_information(RESULT _pulp_ram_mib QUERY TOTAL_PHYSICAL_MEMORY)
        pulp_link_pool_default_depth(${_pulp_ram_mib} _pulp_link_jobs)
    elseif(PULP_LINK_JOBS MATCHES "^[0-9]+$")
        set(_pulp_link_jobs ${PULP_LINK_JOBS})
    else()
        message(FATAL_ERROR "PULP_LINK_JOBS must be empty or a non-negative integer, got '${PULP_LINK_JOBS}'")
    endif()
    if(_pulp_link_jobs GREATER 0)
        set_property(GLOBAL APPEND PROPERTY JOB_POOLS pulp_link=${_pulp_link_jobs})
        set(CMAKE_JOB_POOL_LINK pulp_link)
        message(STATUS "Pulp: Ninja link pool pulp_link=${_pulp_link_jobs}")
    endif()
endif()
