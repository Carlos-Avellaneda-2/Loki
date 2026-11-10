# Command pattern: every model mutation is a reversible Command executed through UndoStack.
add_library(loki_commands STATIC
    clip_commands.h
    clip_commands.cpp
    command.h
    command.cpp
    edit_commands.h
    effect_commands.h
    effect_commands.cpp
    edit_commands.cpp
    marker_commands.h
    marker_commands.cpp
    project_commands.h
    project_commands.cpp
    transition_commands.h
    transition_commands.cpp
    undo_stack.h
    undo_stack.cpp
)
add_library(loki::commands ALIAS loki_commands)

target_include_directories(loki_commands PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_commands PUBLIC loki::model)
loki_set_warnings(loki_commands)
