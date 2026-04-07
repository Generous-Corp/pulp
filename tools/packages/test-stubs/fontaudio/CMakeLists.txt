cmake_minimum_required(VERSION 3.20)
project(test-fontaudio CXX)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)
FetchContent_Declare(
  fontaudio
  GIT_REPOSITORY https://github.com/fefanto/fontaudio.git
  GIT_TAG        1.1
  GIT_SHALLOW    TRUE
)
FetchContent_Populate(fontaudio)

# fontaudio is a font asset — verify font file exists
if(NOT EXISTS "${fontaudio_SOURCE_DIR}/font/fontaudio.ttf")
  message(FATAL_ERROR "fontaudio.ttf not found")
endif()

add_executable(test-pkg main.cpp)
target_compile_definitions(test-pkg PRIVATE
  FONTAUDIO_TTF="${fontaudio_SOURCE_DIR}/font/fontaudio.ttf"
)
