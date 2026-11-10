# Analyses that read media frames in the background: stabilizer motion and mask tracking (Phase 14).
add_library(loki_analysis STATIC
    motion.h
    motion.cpp
)
add_library(loki::analysis ALIAS loki_analysis)

target_include_directories(loki_analysis PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_analysis PUBLIC loki::media loki::model loki::render_core loki::util)
loki_set_warnings(loki_analysis)
