# Playback: media clock (audio master), transport (play/shuttle/scrub/step), A/V sync.
add_library(loki_playback STATIC
    playback_clock.h
    playback_clock.cpp
    playback_engine.h
    playback_engine.cpp
    performance.h
    performance.cpp
    sources.h
    sources.cpp
)
add_library(loki::playback ALIAS loki_playback)

target_include_directories(loki_playback PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_playback PUBLIC loki::audio loki::media loki::model loki::render_core loki::text loki::util)
loki_set_warnings(loki_playback)
