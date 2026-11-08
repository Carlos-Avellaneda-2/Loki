# Media I/O: probing, demux/decode (FFmpeg, hardware with software fallback), frame cache,
# thumbnails, waveforms, disk cache. Pure C++ (no Qt).
add_library(loki_media STATIC
    audio_decoder.h
    audio_decoder.cpp
    ffmpeg_utils.h
    ffmpeg_utils.cpp
    frame_cache.h
    frame_cache.cpp
    media_cache.h
    media_cache.cpp
    media_probe.h
    media_probe.cpp
    thumbnail.h
    thumbnail.cpp
    video_decoder.h
    video_decoder.cpp
    video_frame.h
    waveform.h
    waveform.cpp
)
add_library(loki::media ALIAS loki_media)

target_include_directories(loki_media PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_media PUBLIC loki::model loki::util loki::ffmpeg)
loki_set_warnings(loki_media)
