# Deprecated compatibility wrapper.
#
# The real implementation lives in PulpUtils.cmake for both in-tree and
# installed-SDK consumers. Keep this include-only shim for callers that still
# reference PulpPlugin.cmake directly.

message(DEPRECATION
    "PulpPlugin.cmake is deprecated; include PulpUtils.cmake via find_package(Pulp) instead.")

include("${CMAKE_CURRENT_LIST_DIR}/PulpUtils.cmake")
