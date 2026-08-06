# Stage only the distributable Forge Modular generator toolchain.
#
# This runs from bundle POST_BUILD commands, where copying the source directory
# verbatim would also copy ignored machine-local material such as `.corpus/`.
# SOURCE and DEST are explicit so the same rule can be exercised in isolation.
if(NOT DEFINED SOURCE OR NOT IS_DIRECTORY "${SOURCE}")
    message(FATAL_ERROR "SOURCE must name the tools/rack directory")
endif()
if(NOT DEFINED DEST OR DEST STREQUAL "")
    message(FATAL_ERROR "DEST must name the staged tools/rack directory")
endif()

file(REMOVE_RECURSE "${DEST}")
file(MAKE_DIRECTORY "${DEST}")
file(COPY "${SOURCE}/" DESTINATION "${DEST}"
    PATTERN ".corpus" EXCLUDE
    PATTERN ".sweeps" EXCLUDE
    PATTERN ".git" EXCLUDE
    PATTERN "__pycache__" EXCLUDE
    PATTERN ".pytest_cache" EXCLUDE
    PATTERN ".DS_Store" EXCLUDE)

# Keep this as a second, independent boundary. A future edit that weakens the
# copy exclusions must turn the build red instead of quietly redistributing a
# fetched book or a nested repository inside a signed application.
file(GLOB_RECURSE _staged_entries LIST_DIRECTORIES true
    "${DEST}/*" "${DEST}/.*")
foreach(_entry IN LISTS _staged_entries)
    get_filename_component(_name "${_entry}" NAME)
    if(_name STREQUAL ".corpus" OR _name STREQUAL ".sweeps" OR
       _name STREQUAL ".git" OR _name STREQUAL "__pycache__" OR
       _name STREQUAL ".pytest_cache")
        message(FATAL_ERROR "unshippable material survived staging: ${_entry}")
    endif()
endforeach()
