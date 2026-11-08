# Project-wide compiler settings and the strict warning set applied to Loki's own targets.

if(MSVC)
    # fmt/spdlog require UTF-8 source handling; __cplusplus must report the real standard.
    add_compile_options(/utf-8 /Zc:__cplusplus /permissive- /MP)
    add_compile_definitions(NOMINMAX WIN32_LEAN_AND_MEAN UNICODE _UNICODE)
endif()

# Applies the strict warning set (warnings are errors) to a Loki target.
# Third-party headers are included as SYSTEM/external and excluded.
function(loki_set_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE
            /W4 /WX
            /wd4324 # structure padded due to alignas (intentional: false-sharing avoidance)
            /external:anglebrackets /external:W0
            /w14242 # conversion, possible loss of data
            /w14254 # larger bit field assigned to smaller
            /w14263 # member function does not override base virtual
            /w14265 # class has virtual functions but non-virtual destructor
            /w14287 # unsigned/negative constant mismatch
            /w14296 # expression is always true/false
            /w14311 # pointer truncation
            /w14826 # conversion is sign-extended
            /w14905 /w14906 # string literal casts
            /w14928 # illegal copy-initialization
        )
    else()
        target_compile_options(${target} PRIVATE
            -Wall -Wextra -Wpedantic -Werror
            -Wshadow -Wnon-virtual-dtor -Wold-style-cast -Wcast-align
            -Woverloaded-virtual -Wnull-dereference -Wdouble-promotion
        )
    endif()
endfunction()
