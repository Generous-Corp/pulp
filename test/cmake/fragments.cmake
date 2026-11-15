# Registration fragments: one file per new test registration, picked up here.
#
# Appending a block to a shared manifest (mac_platform_tests.cmake, for one)
# made two pull requests that each added a block at the tail conflict on the
# same lines, and the union of both sides is not a repair: it interleaves the
# two `if()` ... `endif()` bodies or repeats a block, so it configures wrongly
# or not at all. A new registration goes in its own file instead:
#
#     test/cmake/fragments/<area>-<what>.cmake
#
# Two pull requests then add two different files and never touch the same line.
# Fragments are included after every owner manifest, in lexical order, so a
# fragment may use any target or helper those manifests define. Name a fragment
# for its subject, not its order: nothing may depend on one fragment running
# before another. CONFIGURE_DEPENDS makes a new or removed fragment re-run
# configure without touching this file.
#
# Existing registrations stay where they are. Move one into a fragment only when
# you are already changing it.
file(GLOB _pulp_test_fragments CONFIGURE_DEPENDS
    "${CMAKE_CURRENT_LIST_DIR}/fragments/*.cmake")
list(SORT _pulp_test_fragments)
foreach(_pulp_test_fragment IN LISTS _pulp_test_fragments)
    include("${_pulp_test_fragment}")
endforeach()
unset(_pulp_test_fragments)
unset(_pulp_test_fragment)
