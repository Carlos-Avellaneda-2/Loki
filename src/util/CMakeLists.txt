add_library(loki_util STATIC
    logging.h
    logging.cpp
)
add_library(loki::util ALIAS loki_util)

target_include_directories(loki_util PUBLIC "${PROJECT_SOURCE_DIR}/src")
target_link_libraries(loki_util PUBLIC spdlog::spdlog)
loki_set_warnings(loki_util)
