cmake_minimum_required(VERSION 3.21)

project(platform_h LANGUAGES CXX VERSION 0.1.0)

add_library(platform STATIC
    platform.h
    platform_common.cpp
)
add_library(platform::platform ALIAS platform)

target_compile_features(platform PUBLIC cxx_std_20)
target_include_directories(platform PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}")

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "The Win32 backend currently requires MSVC.")
    endif()

    enable_language(RC)

    target_sources(platform PRIVATE platform_win.cpp platform_win_audio.cpp)

    # WIN32_LEAN_AND_MEAN / NOMINMAX are defined by platform_win.cpp itself;
    # defining them here as well would be a macro redefinition.
    target_compile_definitions(platform PUBLIC WIN32 _WINDOWS UNICODE _UNICODE)

    # platform_win.cpp still carries #pragma comment(lib, ...) for these; naming
    # them here is what a non-MSVC backend will rely on once the pragmas go.
    target_link_libraries(platform PUBLIC
        wininet shell32 comdlg32 shlwapi windowscodecs winhttp comctl32
        ole32 oleaut32 user32 gdi32 advapi32 xaudio2
    )
else()
    message(FATAL_ERROR "platform-h has a Win32 backend only; add src for this platform first.")
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform_app.cmake")

# Only when built directly; apps that pull this in with FetchContent do not want
# the test target in their build.
if(PROJECT_IS_TOP_LEVEL)
    enable_testing()

    platform_add_app(platform_tests CONSOLE
        SOURCES     tests/platform_tests.cpp
        DESCRIPTION "platform-h unit tests"
        EMBED       tests/data/sample.css
    )

    add_test(NAME platform COMMAND platform_tests)
endif()
