# Rendering.
#  - loki_render_core: pure description of composites (layers, transforms, blend math). No Qt, so
#    playback and export can build composites without OpenGL.
#  - loki_render: OpenGL 3.3 implementation (frame renderer, multitrack compositor) via Qt.
add_library(loki_render_core STATIC
    color_conversion.h
    color_conversion.cpp
    composite.h
    composite.cpp
)
add_library(loki::render_core ALIAS loki_render_core)
target_include_directories(loki_render_core PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_render_core PUBLIC loki::media loki::model)
loki_set_warnings(loki_render_core)

add_library(loki_render STATIC
    gl_compositor.h
    gl_compositor.cpp
    gl_frame_renderer.h
    gl_frame_renderer.cpp
)
add_library(loki::render ALIAS loki_render)
target_include_directories(loki_render PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_render PUBLIC loki::render_core Qt6::Gui Qt6::OpenGL)
loki_set_warnings(loki_render)
