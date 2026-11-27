add_library(helper_import INTERFACE IMPORTED)
set_property(TARGET helper_import PROPERTY INTERFACE_LINK_LIBRARIES pulp::canvas)
add_library(route_scoped INTERFACE)
target_link_libraries(route_scoped INTERFACE helper_import)
