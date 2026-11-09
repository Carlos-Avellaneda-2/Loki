# loki_app: main window, theme and app wiring (linked by the executable and by UI tests).
add_library(loki_app STATIC
    export_dialog.h
    export_dialog.cpp
    main_window.h
    main_window.cpp
    preferences_dialog.h
    preferences_dialog.cpp
    shortcuts_dialog.h
    shortcuts_dialog.cpp
    theme.h
    theme.cpp
    qt_log_bridge.h
    qt_log_bridge.cpp
)
add_library(loki::app ALIAS loki_app)

qt_add_resources(loki_app "loki_theme"
    PREFIX "/loki"
    BASE "resources"
    FILES resources/theme/dark.qss
)

target_include_directories(loki_app PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_app
    PUBLIC
        Qt6::Widgets
        loki::ui
        loki::export
        loki::util
        ads::qtadvanceddocking
)
target_compile_definitions(loki_app PUBLIC LOKI_VERSION="${PROJECT_VERSION}")
loki_set_warnings(loki_app)

qt_add_executable(loki WIN32 main.cpp)
target_link_libraries(loki PRIVATE loki::app)
loki_set_warnings(loki)
