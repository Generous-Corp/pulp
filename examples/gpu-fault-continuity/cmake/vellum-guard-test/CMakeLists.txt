# Configure-time check of pulp_gpu_fault_refuse_vellum(): with
# -DWITH_VELLUM=ON it must refuse, without it it must configure.
cmake_minimum_required(VERSION 3.22)
project(gpu_fault_vellum_guard_test NONE)
option(WITH_VELLUM "Declare a Vellum::Gpu target" OFF)
if(WITH_VELLUM)
    add_library(Vellum::Gpu INTERFACE IMPORTED)
endif()
include(${CMAKE_CURRENT_LIST_DIR}/../gpu_fault_guards.cmake)
pulp_gpu_fault_refuse_vellum()
message(STATUS "gpu-fault vellum guard: configured")
