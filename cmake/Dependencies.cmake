# Third-party dependencies.
#  - vcpkg (manifest mode, vcpkg.json): spdlog, nlohmann-json, GTest. FFmpeg/miniaudio join in later phases.
#  - Qt 6: prebuilt LGPL binaries (aqtinstall locally, install-qt-action in CI), found via CMAKE_PREFIX_PATH.
#  - Qt Advanced Docking System: built from source with FetchContent as a shared library (LGPL-2.1),
#    because the vcpkg port would pull Qt from vcpkg as well.

include(FetchContent)

find_package(spdlog CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

find_package(Qt6 6.8 REQUIRED COMPONENTS Core Gui Widgets Concurrent)
qt_standard_project_setup()

set(BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
set(BUILD_STATIC OFF CACHE BOOL "" FORCE)
FetchContent_Declare(qtads
    GIT_REPOSITORY https://github.com/githubuser0xFFFF/Qt-Advanced-Docking-System.git
    GIT_TAG        023ce95934fecfd4cf5c672c9c404fabe0f54923 # v5.1.1
    GIT_SHALLOW    FALSE
    SYSTEM
    EXCLUDE_FROM_ALL)
FetchContent_MakeAvailable(qtads)
# ADS forces its own <build>/x64/bin output folder; put the DLL next to Loki's executables instead.
set_target_properties(qtadvanceddocking-qt6 PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}"
    RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")

if(LOKI_BUILD_TESTS)
    find_package(GTest CONFIG REQUIRED)
    find_package(Qt6 REQUIRED COMPONENTS Test)
endif()
