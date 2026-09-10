# Public headers match the FFmpeg 7.1 ABI shipped with the supported Qt kit.
# FFmpeg itself is dynamically loaded; no second decoder build or executable is needed.
if(CMAKE_CXX_BYTE_ORDER STREQUAL "BIG_ENDIAN")
    set(WHQ_FFMPEG_BIG_ENDIAN 1)
else()
    set(WHQ_FFMPEG_BIG_ENDIAN 0)
endif()

set(WHQ_FFMPEG_FAST_UNALIGNED 0)
if(CMAKE_SYSTEM_PROCESSOR MATCHES "^(AMD64|amd64|x86_64|i.86)$")
    set(WHQ_FFMPEG_FAST_UNALIGNED 1)
endif()

configure_file(
    "${CMAKE_CURRENT_LIST_DIR}/ffmpeg_avconfig.h.in"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ffmpeg/libavutil/avconfig.h"
    @ONLY
)
target_include_directories(WhqMusic SYSTEM PRIVATE
    "${PROJECT_SOURCE_DIR}/third_party/ffmpeg/include"
    "${CMAKE_CURRENT_BINARY_DIR}/generated/ffmpeg"
)

if(WIN32)
    get_filename_component(_whq_qt_bin "${Qt6_DIR}/../../../bin" ABSOLUTE)
    set(WHQ_FFMPEG_RUNTIME_DIR "${_whq_qt_bin}" CACHE PATH
        "Directory containing Qt-compatible FFmpeg 7.1 runtime DLLs")
    foreach(_whq_library IN ITEMS avutil-59 swresample-5 avcodec-61 avformat-61)
        set(_whq_dll "${WHQ_FFMPEG_RUNTIME_DIR}/${_whq_library}.dll")
        if(NOT EXISTS "${_whq_dll}")
            message(FATAL_ERROR
                "Missing FFmpeg runtime: ${_whq_dll}. Set WHQ_FFMPEG_RUNTIME_DIR to the Qt FFmpeg 7.1 bin directory.")
        endif()
        add_custom_command(TARGET WhqMusic POST_BUILD
            COMMAND "${CMAKE_COMMAND}" -E copy_if_different
                "${_whq_dll}" "$<TARGET_FILE_DIR:WhqMusic>"
            VERBATIM
        )
    endforeach()
endif()

add_custom_command(TARGET WhqMusic POST_BUILD
    COMMAND "${CMAKE_COMMAND}" -E make_directory
        "$<TARGET_FILE_DIR:WhqMusic>/licenses/ffmpeg"
    COMMAND "${CMAKE_COMMAND}" -E copy_if_different
        "${PROJECT_SOURCE_DIR}/third_party/ffmpeg/COPYING.LGPLv2.1"
        "${PROJECT_SOURCE_DIR}/third_party/ffmpeg/README.md"
        "$<TARGET_FILE_DIR:WhqMusic>/licenses/ffmpeg"
    VERBATIM
)
