# The runtime content resize must keep the native window's top-left corner.
# Initial launch centering is separate and remains allowed in the constructor.

if(NOT DEFINED PULP_SOURCE_DIR)
    message(FATAL_ERROR "PULP_SOURCE_DIR is required")
endif()

if(DEFINED PULP_SDL_WINDOW_HOST_SOURCE)
    set(_source_path "${PULP_SDL_WINDOW_HOST_SOURCE}")
else()
    set(_source_path "${PULP_SOURCE_DIR}/core/view/src/sdl_window_host.cpp")
endif()

file(READ "${_source_path}" _source)
string(FIND "${_source}" "void request_content_size" _start)
string(FIND "${_source}" "void set_fixed_aspect_ratio" _end)
if(_start LESS 0 OR _end LESS 0 OR _end LESS_EQUAL _start)
    message(FATAL_ERROR "could not locate SdlWindowHost resize methods")
endif()

math(EXPR _length "${_end} - ${_start}")
string(SUBSTRING "${_source}" ${_start} ${_length} _resize)
if(_resize MATCHES "SDL_SetWindowPosition")
    message(FATAL_ERROR
        "request_content_size must resize in place; it must not reposition the window")
endif()
if(NOT _resize MATCHES "SDL_SetWindowSize")
    message(FATAL_ERROR "request_content_size must call SDL_SetWindowSize")
endif()
