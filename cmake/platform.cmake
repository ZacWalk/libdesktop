# libdesktop-platform: OS services and application-facing platform facilities.

set(libdesktop_platform_headers
    include/libdesktop/platform.h
    include/libdesktop/platform/resources.h
    include/libdesktop/platform/app.h
    include/libdesktop/platform/environment.h
    include/libdesktop/platform/file.h
    include/libdesktop/platform/input.h
    include/libdesktop/platform/window.h
    include/libdesktop/platform/media.h
    include/libdesktop/platform/services.h
    include/libdesktop/platform/tasks.h
    include/libdesktop/platform/process.h
    include/libdesktop/platform/image.h
    include/libdesktop/platform/web.h
    include/libdesktop/platform/text.h
    include/libdesktop/platform/shell.h
    include/libdesktop/platform/dialogs.h
)

add_library(libdesktop-platform STATIC
    ${libdesktop_platform_headers}
    src/platform/platform_common.cpp
    src/platform/keys.cpp
    src/platform/resources.cpp
    src/platform/tasks.cpp
    src/platform/audio_mixer.cpp)
add_library(libdesktop::platform ALIAS libdesktop-platform)

target_compile_features(libdesktop-platform PUBLIC cxx_std_20)
target_link_libraries(libdesktop-platform PUBLIC libdesktop::core)
target_include_directories(libdesktop-platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_precompile_headers(libdesktop-platform PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/platform.h")

if(MSVC)
    target_compile_options(libdesktop-platform PUBLIC /utf-8 PRIVATE /bigobj /W3 /sdl)
    libdesktop_enable_msvc_exceptions(libdesktop-platform)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(libdesktop-platform PRIVATE -Wall -Wno-unknown-pragmas -Wno-multichar)
endif()

if(WIN32)
    target_sources(libdesktop-platform PRIVATE
        include/libdesktop/platform/win32.h
        src/platform/win/os.h
        src/platform/win/os_text.h
        src/platform/win/platform.cpp
        src/platform/win/files.cpp
        src/platform/win/process.cpp
        src/platform/win/settings.cpp
        src/platform/win/share.cpp
        src/platform/win/web.cpp
        src/platform/win/os.cpp
        src/platform/win/os_audio.cpp
        src/platform/win/sound.cpp
        src/platform/win/movie_writer.cpp
        src/platform/win/window.cpp
        src/platform/win/app.cpp
        src/platform/win/child_process.cpp
        src/platform/win/image.cpp
        src/platform/win/compound_preview.cpp
        src/platform/win/async_web.cpp
        src/platform/win/text_encoding.cpp
        src/platform/win/shell.cpp
        src/platform/win/dialogs.cpp
        src/platform/win/file_io.cpp
    )

    # Cursor files are application-independent platform assets. Convert them while configuring so
    # the platform archive owns their bytes rather than requiring every executable to include an RC.
    set(libdesktop_platform_cursor_cpp "${CMAKE_CURRENT_BINARY_DIR}/libdesktop_platform_cursors.cpp")
    set(libdesktop_platform_cursor_text "// Generated from src/platform/win/res by CMake. Do not edit.\n#include <cstddef>\n\n")
    foreach(pair IN ITEMS
            "color_select=color_select.cur"
            "hand=hand.cur"
            "hand_down=hand_down.cur"
            "hand_up=hand_up.cur"
            "move=move.cur"
            "select=select.cur"
            "zoom=zoom.cur")
        string(REPLACE "=" ";" pair "${pair}")
        list(GET pair 0 symbol)
        list(GET pair 1 file)
        set(path "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win/res/${file}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
        file(READ "${path}" hex HEX)
        string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1," bytes "${hex}")
        string(LENGTH "${hex}" hex_length)
        math(EXPR length "${hex_length} / 2")
        string(APPEND libdesktop_platform_cursor_text
            "extern const unsigned char libdesktop_cursor_${symbol}[] = {${bytes}};\n"
            "extern const std::size_t libdesktop_cursor_${symbol}_size = ${length};\n")
    endforeach()
    file(WRITE "${libdesktop_platform_cursor_cpp}.in" "${libdesktop_platform_cursor_text}")
    configure_file("${libdesktop_platform_cursor_cpp}.in" "${libdesktop_platform_cursor_cpp}" COPYONLY)
    set_source_files_properties("${libdesktop_platform_cursor_cpp}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_sources(libdesktop-platform PRIVATE "${libdesktop_platform_cursor_cpp}")

    target_include_directories(libdesktop-platform PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win"
        "${CMAKE_CURRENT_SOURCE_DIR}/src/core/win")
    target_compile_definitions(libdesktop-platform PRIVATE UNICODE _UNICODE)

    option(LIBDESKTOP_STORE_PACKAGE "Build libdesktop-platform for a Store package (defines WINSTORE)" OFF)
    if(LIBDESKTOP_STORE_PACKAGE)
        target_compile_definitions(libdesktop-platform PUBLIC WINSTORE)
    endif()

    target_link_libraries(libdesktop-platform PUBLIC
        user32 gdi32 shell32 comdlg32 ole32 oleaut32 wininet winhttp ws2_32 avrt
        mfplat mfreadwrite mfuuid winmm dwmapi imm32 uxtheme comctl32 wtsapi32 shlwapi shcore version)
else()
    target_include_directories(libdesktop-platform PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/src/core/linux")
    target_sources(libdesktop-platform PRIVATE
        src/platform/linux/platform.cpp
        src/platform/linux/files.cpp
        src/platform/linux/settings.cpp
        src/platform/linux/desktop.cpp
        src/platform/linux/window.cpp
        src/platform/linux/app.cpp
        src/platform/linux/child_process.cpp
        src/platform/linux/image.cpp
        src/platform/linux/services.cpp
        src/platform/linux/shell.cpp
    )
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdl3.cmake")
    target_link_libraries(libdesktop-platform PRIVATE SDL3::SDL3-static)
    find_package(Threads REQUIRED)
    target_link_libraries(libdesktop-platform PUBLIC Threads::Threads)
endif()
