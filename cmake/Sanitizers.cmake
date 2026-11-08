# AddressSanitizer support (LOKI_ENABLE_ASAN). Applied globally so every module, including
# third-party code built from source, is instrumented consistently.

if(NOT LOKI_ENABLE_ASAN)
    return()
endif()

if(MSVC)
    # /RTC* is incompatible with ASan; the asan preset uses RelWithDebInfo which has no /RTC.
    add_compile_options(/fsanitize=address /Zi)
    add_link_options(/INCREMENTAL:NO /DEBUG)

    # Copy the ASan runtime next to the binaries so the app and tests start without PATH tweaks.
    get_filename_component(_loki_cl_dir "${CMAKE_CXX_COMPILER}" DIRECTORY)
    file(GLOB _loki_asan_dlls "${_loki_cl_dir}/clang_rt.asan_dynamic-x86_64.dll"
                              "${_loki_cl_dir}/clang_rt.asan_dbg_dynamic-x86_64.dll")
    if(NOT _loki_asan_dlls)
        message(FATAL_ERROR "ASan runtime not found next to ${CMAKE_CXX_COMPILER}. "
                            "Install the MSVC component 'C++ AddressSanitizer'.")
    endif()
    file(COPY ${_loki_asan_dlls} DESTINATION "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}")
else()
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

message(STATUS "Loki: AddressSanitizer enabled")
