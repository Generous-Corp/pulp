# Offline driver that asks a built LV2 module to describe itself. Host-side and
# tiny on purpose: it links nothing from the plugin, it dlopens it, so one
# binary serves every Pulp LV2 target.
add_executable(pulp-lv2-ttlgen main.cpp)
target_compile_features(pulp-lv2-ttlgen PRIVATE cxx_std_20)
if(NOT WIN32)
    target_link_libraries(pulp-lv2-ttlgen PRIVATE ${CMAKE_DL_LIBS})
endif()
