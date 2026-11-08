add_library(loki_ui STATIC
    media_services.h
    media_services.cpp
    panel_id.h
    panel_factory.h
    panel_factory.cpp
    placeholder_panel.h
    placeholder_panel.cpp
    project_controller.h
    project_controller.cpp
    project_item_model.h
    project_item_model.cpp
    project_panel.h
    project_panel.cpp
    scrub_bar.h
    scrub_bar.cpp
    source_monitor_panel.h
    source_monitor_panel.cpp
    video_view.h
    video_view.cpp
)
add_library(loki::ui ALIAS loki_ui)

target_include_directories(loki_ui PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_ui PUBLIC Qt6::Widgets Qt6::Concurrent Qt6::OpenGLWidgets loki::commands loki::media loki::playback loki::render loki::model loki::util)
loki_set_warnings(loki_ui)
