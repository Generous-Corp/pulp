# Browser build of the SuperConvolver editor — Pulp's own View tree, painted by
# Skia Ganesh on WebGL2, in a canvas element.
#
# The module is DSP-FREE: it links no Processor, no core/audio, no core/format,
# no core/gpu_audio. It talks to whatever host mounted it over the web-player
# HostAdapter seam, so the SAME .wasm serves the WAM demo and the WebCLAP demo.
#
# Build:
#   source ~/emsdk/emsdk_env.sh
#   emcmake cmake -S examples/web-demos/super-convolver-ui -B build-webui \
#       -DCMAKE_BUILD_TYPE=Release \
#       -DSKIA_DIR=<wasm Skia slice root> \
#       -DPULP_WEBUI_CHOC_INCLUDE=<dir containing choc/>
#   cmake --build build-webui
#
# Output: build-webui/PulpSuperConvolverUi.{js,wasm,data}
#
# The source subset, link flags, and export table live in
# tools/cmake/PulpWebUi.cmake — this file only declares the target.

cmake_minimum_required(VERSION 3.20)
project(pulp-super-convolver-ui LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

set(PULP_ROOT "${CMAKE_CURRENT_SOURCE_DIR}/../../..")
include(${PULP_ROOT}/tools/cmake/PulpWebUi.cmake)

pulp_add_web_ui(PulpSuperConvolverUi
    ENTRY    ${CMAKE_CURRENT_SOURCE_DIR}/ui_entry.cpp
    INCLUDES ${CMAKE_CURRENT_SOURCE_DIR})
