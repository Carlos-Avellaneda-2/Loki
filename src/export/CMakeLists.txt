# Export (Phase 10): the preview's composite description rendered offscreen (GPU, CPU fallback)
# and encoded with FFmpeg (LGPL encoders only, D23).
add_library(loki_export STATIC
    encoders.h
    export_job.h
    export_job.cpp
    media_writer.h
    media_writer.cpp
)
add_library(loki::export ALIAS loki_export)

target_include_directories(loki_export PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_export PUBLIC loki::playback loki::render PRIVATE loki::ffmpeg Qt6::Gui Qt6::OpenGL)
loki_set_warnings(loki_export)
