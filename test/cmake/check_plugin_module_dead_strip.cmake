# Verifies that an Apple plug-in module was linked with -dead_strip in an
# optimized configuration.
#
# Inputs:
#   OPTIONS  the module target's evaluated LINK_OPTIONS
#   CONFIG   the active build configuration
#   MODULE   the module target name, for messages
if(CONFIG STREQUAL "Debug")
    message(STATUS "${MODULE}: Debug configuration, dead-strip not expected")
    return()
endif()
string(FIND "${OPTIONS}" "-dead_strip" _pulp_dead_strip_at)
if(_pulp_dead_strip_at EQUAL -1)
    message(FATAL_ERROR "${MODULE} is not linked with -dead_strip (LINK_OPTIONS: ${OPTIONS})")
endif()
# -dead_strip_dylibs is a different flag; make sure the match is the real one.
string(REPLACE "-dead_strip_dylibs" "" _pulp_without_dylibs "${OPTIONS}")
string(FIND "${_pulp_without_dylibs}" "-dead_strip" _pulp_dead_strip_at)
if(_pulp_dead_strip_at EQUAL -1)
    message(FATAL_ERROR "${MODULE} only carries -dead_strip_dylibs, not -dead_strip")
endif()
message(STATUS "${MODULE}: linked with -dead_strip")
