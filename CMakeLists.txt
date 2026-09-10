cmake_minimum_required(VERSION 3.21)

project(WhqMusic VERSION 0.1.0 LANGUAGES CXX)

include(FetchContent)

set(BUILD_SHARED_LIBS OFF CACHE BOOL "Build static dependencies" FORCE)
set(BUILD_TESTING OFF CACHE BOOL "Disable dependency tests" FORCE)
set(TAGLIB_BUILD_EXAMPLES OFF CACHE BOOL "Disable TagLib examples" FORCE)

FetchContent_Declare(
    taglib
    GIT_REPOSITORY https://github.com/taglib/taglib.git
    GIT_TAG v2.1.1
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(taglib)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

set(CMAKE_AUTOMOC ON)
set(CMAKE_AUTOUIC ON)
set(CMAKE_AUTORCC ON)

find_package(Qt6 REQUIRED COMPONENTS Concurrent Multimedia Network Sql Widgets)

qt_add_executable(WhqMusic
    src/main.cpp
    src/audio_effect_processor.cpp
    src/audio_effect_processor.h
    src/audio_player.cpp
    src/audio_player.h
    src/audio_decoder_worker.cpp
    src/audio_decoder_worker.h
    src/ffmpeg_api.cpp
    src/ffmpeg_api.h
    src/audio_playback_progress.cpp
    src/audio_playback_progress.h
    src/main_window.cpp
    src/main_window.h
    src/app_dialog.cpp
    src/app_dialog.h
    src/lyrics_download_dialog.cpp
    src/lyrics_download_dialog.h
    src/audio_file_editor.cpp
    src/audio_file_editor.h
    src/library_widgets.cpp
    src/lyrics_widgets.cpp
    src/main_window_groups.cpp
    src/main_window_library.cpp
    src/main_window_lyrics.cpp
    src/main_window_metadata.cpp
    src/main_window_theme.cpp
    src/main_window_tray.cpp
    src/main_window_ui.cpp
    src/metadata_scanner.cpp
    src/metadata_scanner.h
    src/music_file_utils.cpp
    src/music_file_utils.h
    src/music_library_repository.cpp
    src/music_library_repository.h
    src/music_library_service.cpp
    src/music_library_service.h
    src/music_types.h
    src/playback_controller.cpp
    src/playback_controller.h
    src/player_surface.cpp
    src/player_widgets.cpp
    src/player_widgets.h
    src/tray_widgets.cpp
    src/main_window_pages.cpp
    src/main_window_settings.cpp
    src/main_window_audio.cpp
    src/main_window_playback.cpp
    resources/resources.qrc
)

include(cmake/ffmpeg.cmake)

if(WIN32)
    target_sources(WhqMusic PRIVATE resources/app_icon.rc)
    target_link_libraries(WhqMusic PRIVATE ole32 uuid user32)
endif()

target_link_libraries(WhqMusic PRIVATE Qt6::Concurrent Qt6::Multimedia Qt6::Network Qt6::Sql Qt6::Widgets tag)

target_include_directories(
    WhqMusic
    PRIVATE
        ${taglib_SOURCE_DIR}/taglib
        ${taglib_SOURCE_DIR}/taglib/toolkit
        ${taglib_BINARY_DIR}
)

set_target_properties(WhqMusic PROPERTIES
    WIN32_EXECUTABLE ON
    MACOSX_BUNDLE ON
)
