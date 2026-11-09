# Document model. Pure C++: no Qt dependency (D1), read-only public API (D2).
add_library(loki_model STATIC
    effect_registry.h
    effect_registry.cpp
    ids.h
    keyframe.h
    keyframe.cpp
    project.h
    project.cpp
    project_json.h
    project_json.cpp
    transitions.h
    transitions.cpp
    types.h
)
add_library(loki::model ALIAS loki_model)

target_include_directories(loki_model PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_model PUBLIC loki::util nlohmann_json::nlohmann_json)
loki_set_warnings(loki_model)
