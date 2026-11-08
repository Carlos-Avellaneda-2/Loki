cmake_minimum_required(VERSION 3.25)

project(Loki
    VERSION 0.1.0
    DESCRIPTION "Non-linear video editor"
    LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Executables and DLLs (Qt ADS, sanitizer runtime) share one folder so the app and tests run in place.
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/lib")

option(LOKI_BUILD_TESTS "Build unit tests" ON)
option(LOKI_ENABLE_ASAN "Build with AddressSanitizer" OFF)

list(APPEND CMAKE_MODULE_PATH "${CMAKE_CURRENT_SOURCE_DIR}/cmake")
include(CompilerWarnings)
include(Sanitizers)
include(Dependencies)

add_subdirectory(src)

if(LOKI_BUILD_TESTS)
    enable_testing()
    add_subdirectory(tests)
endif()
