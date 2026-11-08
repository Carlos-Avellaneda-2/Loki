include(GoogleTest)

# Qt DLLs live outside the build tree (prebuilt Qt); vcpkg DLLs are copied next to each test by
# the vcpkg toolchain. Tests are registered by scanning sources (gtest_add_tests) so CMake never
# has to run a test binary at build time without Qt on PATH.
get_target_property(_loki_qt_core_location Qt6::Core IMPORTED_LOCATION_RELEASE)
if(NOT _loki_qt_core_location)
    get_target_property(_loki_qt_core_location Qt6::Core LOCATION)
endif()
get_filename_component(LOKI_QT_BIN_DIR "${_loki_qt_core_location}" DIRECTORY)

function(loki_add_test_executable target)
    cmake_parse_arguments(ARG "" "" "SOURCES;LIBS" ${ARGN})
    add_executable(${target} ${ARG_SOURCES})
    target_link_libraries(${target} PRIVATE ${ARG_LIBS})
    target_include_directories(${target} PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/support")
    target_compile_definitions(${target} PRIVATE LOKI_TEST_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data")
    loki_set_warnings(${target})
    gtest_add_tests(TARGET ${target} SOURCES ${ARG_SOURCES} TEST_LIST _tests)
    set_tests_properties(${_tests} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${LOKI_QT_BIN_DIR};QT_QPA_PLATFORM=set:offscreen"
        TIMEOUT 120)
endfunction()

loki_add_test_executable(loki_util_tests
    SOURCES
        util/atomic_file_test.cpp
        util/logging_test.cpp
        util/rational_test.cpp
        util/task_pool_test.cpp
        util/timecode_test.cpp
        util/uuid_test.cpp
    LIBS loki::util GTest::gtest GTest::gtest_main)

loki_add_test_executable(loki_model_tests
    SOURCES
        model/keyframe_test.cpp
        model/project_json_test.cpp
        model/project_test.cpp
    LIBS loki::model nlohmann_json::nlohmann_json GTest::gtest GTest::gtest_main)

loki_add_test_executable(loki_commands_tests
    SOURCES
        commands/clip_commands_test.cpp
        commands/project_commands_test.cpp
        commands/random_edit_test.cpp
        commands/undo_stack_test.cpp
    LIBS loki::commands GTest::gtest GTest::gtest_main)

# Synthetic media generated with the linked FFmpeg encoders (D8).
add_library(loki_test_support STATIC
    support/media_assets.h
    support/media_assets.cpp
    support/media_generator.h
    support/media_generator.cpp
)
target_include_directories(loki_test_support PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/support")
target_link_libraries(loki_test_support PUBLIC loki::media GTest::gtest)
loki_set_warnings(loki_test_support)

loki_add_test_executable(loki_media_tests
    SOURCES
        media/audio_decoder_test.cpp
        media/media_cache_test.cpp
        media/media_probe_test.cpp
        media/video_decoder_test.cpp
    LIBS loki_test_support loki::media GTest::gtest GTest::gtest_main)

loki_add_test_executable(loki_playback_tests
    SOURCES
        playback/playback_test.cpp
    LIBS loki_test_support loki::playback loki::render GTest::gtest GTest::gtest_main)

loki_add_test_executable(loki_app_tests
    SOURCES
        app/main_window_test.cpp
        app/theme_test.cpp
        ui/project_controller_test.cpp
        ui/project_panel_test.cpp
        ui/source_monitor_test.cpp
        qt_test_main.cpp
    LIBS loki::app loki_test_support Qt6::Test GTest::gtest)
