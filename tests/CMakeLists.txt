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
    loki_set_warnings(${target})
    gtest_add_tests(TARGET ${target} SOURCES ${ARG_SOURCES} TEST_LIST _tests)
    set_tests_properties(${_tests} PROPERTIES
        ENVIRONMENT_MODIFICATION "PATH=path_list_prepend:${LOKI_QT_BIN_DIR};QT_QPA_PLATFORM=set:offscreen"
        TIMEOUT 60)
endfunction()

loki_add_test_executable(loki_util_tests
    SOURCES util/logging_test.cpp
    LIBS loki::util GTest::gtest GTest::gtest_main)

loki_add_test_executable(loki_app_tests
    SOURCES app/main_window_test.cpp app/theme_test.cpp qt_test_main.cpp
    LIBS loki::app GTest::gtest)
