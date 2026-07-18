add_library(pulp-timeline
    src/command.cpp
    src/document_session.cpp
    src/identity_directory.cpp
    src/journal.cpp
    src/model.cpp
    src/transaction.cpp
    src/undo.cpp
)
add_library(pulp::timeline ALIAS pulp-timeline)

target_include_directories(pulp-timeline
    PUBLIC
        $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
        $<INSTALL_INTERFACE:include>
)

target_link_libraries(pulp-timeline
    PUBLIC
        pulp::runtime
        pulp::timebase
)
