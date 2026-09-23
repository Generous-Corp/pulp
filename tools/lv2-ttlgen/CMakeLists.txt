# Offline driver that asks a built LV2 module to describe itself. Host-side and
# tiny on purpose: it links nothing from the plugin, it dlopens it, so one
# binary serves every Pulp LV2 target.
add_executable(pulp-lv2-ttlgen main.cpp)
target_compile_features(pulp-lv2-ttlgen PRIVATE cxx_std_20)
# Header-only dynamic-loader shim: Windows has no <dlfcn.h>, and this is the
# same header the host loaders use. Include path only -- nothing is linked.
target_include_directories(pulp-lv2-ttlgen PRIVATE ${PROJECT_SOURCE_DIR}/core/host/include)
if(NOT WIN32)
    target_link_libraries(pulp-lv2-ttlgen PRIVATE ${CMAKE_DL_LIBS})
endif()
