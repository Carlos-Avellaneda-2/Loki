# Rendering: color conversion (shared CPU/GPU math) and the OpenGL 3.3 frame renderer.
# Phase 6 adds the multitrack compositor and effects behind a backend interface.
add_library(loki_render STATIC
    color_conversion.h
    color_conversion.cpp
    gl_frame_renderer.h
    gl_frame_renderer.cpp
)
add_library(loki::render ALIAS loki_render)

target_include_directories(loki_render PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_render PUBLIC loki::media Qt6::Gui Qt6::OpenGL)
loki_set_warnings(loki_render)
