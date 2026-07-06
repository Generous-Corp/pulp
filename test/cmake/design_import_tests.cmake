# Design import test registrations.
# Included by test/CMakeLists.txt; add focused registrations to the included manifests.

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/design_import_view_widget_tests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/design_import_native_codegen_tests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/design_import_runtime_bridge_tests.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/design_import_tool_cli_tests.cmake")
