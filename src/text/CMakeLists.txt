# Title rasterization with QtGui (D21): QPainter on a private QImage, usable from the media thread.
add_library(loki_text STATIC
    title_renderer.h
    title_renderer.cpp
)
add_library(loki::text ALIAS loki_text)

target_include_directories(loki_text PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_text PUBLIC loki::media PRIVATE Qt6::Gui)
loki_set_warnings(loki_text)
