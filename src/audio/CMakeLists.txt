# Audio output (miniaudio). Phase 8 adds the multitrack mixer here.
add_library(loki_audio STATIC
    audio_device.h
    miniaudio_device.cpp
    effects.h
    effects.cpp
    mixer.h
    mixer.cpp
)
add_library(loki::audio ALIAS loki_audio)

target_include_directories(loki_audio PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_audio PUBLIC loki::util PRIVATE loki::miniaudio)
loki_set_warnings(loki_audio)
