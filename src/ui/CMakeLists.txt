add_library(loki_ui STATIC
    panel_id.h
    panel_factory.h
    panel_factory.cpp
    placeholder_panel.h
    placeholder_panel.cpp
)
add_library(loki::ui ALIAS loki_ui)

target_include_directories(loki_ui PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_ui PUBLIC Qt6::Widgets loki::util)
loki_set_warnings(loki_ui)
