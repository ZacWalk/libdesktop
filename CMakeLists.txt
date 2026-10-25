cmake_minimum_required(VERSION 3.21)

project(libdesktop LANGUAGES CXX VERSION 0.1.0)

# --- libdesktop-system: the foundation and OS services, for Windows and Linux --
# Opt-in for a consumer, so an application that links libdesktop-core alone
# neither fetches nor builds it. Built directly, it is on.
option(LIBDESKTOP_SYSTEM "Build libdesktop-system, the foundation and OS services (docs/system.md)"
    ${PROJECT_IS_TOP_LEVEL})

if(LIBDESKTOP_SYSTEM)
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/system.cmake")

    if(PROJECT_IS_TOP_LEVEL)
        enable_testing()

        # A plain console program on both platforms: platform_add_app is the Win32 backend's.
        add_executable(libdesktop_system_tests tests/system_tests.cpp tests/system_test.h)
        if(WIN32)
            target_sources(libdesktop_system_tests PRIVATE tests/system_win_tests.cpp tests/system_tests.manifest)
        endif()
        target_link_libraries(libdesktop_system_tests PRIVATE libdesktop::system)
        target_precompile_headers(libdesktop_system_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/system.h")
        if(MSVC)
            target_compile_options(libdesktop_system_tests PRIVATE /bigobj /W3)
        else()
            target_compile_options(libdesktop_system_tests PRIVATE -Wall -Wno-unknown-pragmas)
        endif()

        add_test(NAME libdesktop-system COMMAND libdesktop_system_tests)
        set_tests_properties(libdesktop-system PROPERTIES LABELS libdesktop-system)
    endif()
endif()

# --- libdesktop-render: the desktop UI, Windows with Linux stand-ins ------------
# Opt-in like libdesktop-system, which it is written in.
option(LIBDESKTOP_RENDER "Build libdesktop-render, the desktop UI (docs/render.md)" ${PROJECT_IS_TOP_LEVEL})

if(LIBDESKTOP_RENDER)
    if(NOT LIBDESKTOP_SYSTEM)
        message(FATAL_ERROR "libdesktop-render is written in libdesktop-system. Set LIBDESKTOP_SYSTEM.")
    endif()

    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/render.cmake")

    if(PROJECT_IS_TOP_LEVEL)
        add_executable(libdesktop_render_tests tests/render_tests.cpp tests/system_test.h)
        if(WIN32)
            target_sources(libdesktop_render_tests PRIVATE tests/render_win_tests.cpp tests/render_tests.rc
                tests/system_tests.manifest)
            libdesktop_render_resources(tests/render_tests.rc)
        endif()
        target_link_libraries(libdesktop_render_tests PRIVATE libdesktop::render)
        target_precompile_headers(libdesktop_render_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/render.h")
        if(MSVC)
            target_compile_options(libdesktop_render_tests PRIVATE /bigobj /W3)
        else()
            target_compile_options(libdesktop_render_tests PRIVATE -Wall -Wno-unknown-pragmas)
        endif()

        add_test(NAME libdesktop-render COMMAND libdesktop_render_tests)
        set_tests_properties(libdesktop-render PROPERTIES LABELS libdesktop-render)
    endif()
endif()

# libdesktop-core and libdesktop-ui have a Win32 backend only.
if(NOT WIN32)
    if(NOT LIBDESKTOP_SYSTEM)
        message(FATAL_ERROR "libdesktop-core has a Win32 backend only; on this platform only "
            "libdesktop-system builds. Set LIBDESKTOP_SYSTEM.")
    endif()
    return()
endif()

# --- libdesktop-core: the OS abstraction ------------------------------------
add_library(libdesktop-core STATIC
    src/core/platform.h
    src/core/platform_common.cpp
)
add_library(libdesktop::core ALIAS libdesktop-core)

target_compile_features(libdesktop-core PUBLIC cxx_std_20)

# Keeps #include "platform.h" resolving for every consumer after the move.
target_include_directories(libdesktop-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/core")

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "The Win32 backend currently requires MSVC.")
    endif()

    enable_language(RC)

    target_sources(libdesktop-core PRIVATE src/core/platform_win.cpp src/core/platform_win_audio.cpp)

    # WIN32_LEAN_AND_MEAN / NOMINMAX are defined by platform_win.cpp itself;
    # defining them here as well would be a macro redefinition.
    target_compile_definitions(libdesktop-core PUBLIC WIN32 _WINDOWS UNICODE _UNICODE)

    # platform_win.cpp still carries #pragma comment(lib, ...) for these; naming
    # them here is what a non-MSVC backend will rely on once the pragmas go.
    target_link_libraries(libdesktop-core PUBLIC
        wininet shell32 comdlg32 shlwapi windowscodecs winhttp comctl32
        ole32 oleaut32 user32 gdi32 advapi32 xaudio2
    )
else()
    message(FATAL_ERROR "libdesktop has a Win32 backend only; add src for this platform first.")
endif()

# --- libdesktop-ui: reusable widgets, text views and the chat panel ----------
# Opt in with platform_add_app(... UI). An app that does not ask for it does not
# link it, so the games and viewers keep their current size.
add_library(libdesktop-ui STATIC
    src/ui/ui.h
    src/ui/text_types.h
    src/ui/theme.h
    src/ui/theme.cpp
    src/ui/view_host.h
    src/ui/widgets.h
    src/ui/table_layout.h
    src/ui/table_layout.cpp
    src/ui/syntax.h
    src/ui/syntax.cpp
    src/ui/spell.h
    src/ui/spell.cpp
    src/ui/text_line.h
    src/ui/text_buffer.h
    src/ui/text_buffer.cpp
    src/ui/text_input.h
    src/ui/markdown.h
    src/ui/view_base.h
    src/ui/view_text.h
    src/ui/view_doc.h
    src/ui/view_markdown.h
    src/ui/view_csv.h
    src/ui/view_hex.h
    src/ui/view_list.h
    src/ui/view_composer.h
    src/ui/pane_host.h
    src/ui/test_support.h
)
add_library(libdesktop::ui ALIAS libdesktop-ui)

target_link_libraries(libdesktop-ui PUBLIC libdesktop::core)

# Reached as #include "ui/widgets.h", so generic names like widgets.h or theme.h
# cannot collide with an application's own headers.
target_include_directories(libdesktop-ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform_app.cmake")

# Only when built directly; apps that pull this in with FetchContent do not want
# the test target in their build.
if(PROJECT_IS_TOP_LEVEL)
    enable_testing()

    platform_add_app(libdesktop_tests CONSOLE
        SOURCES     tests/platform_tests.cpp
        DESCRIPTION "libdesktop unit tests"
        EMBED       tests/data/sample.css
    )

    add_test(NAME libdesktop COMMAND libdesktop_tests)
    set_tests_properties(libdesktop PROPERTIES LABELS libdesktop)

    platform_add_app(libdesktop_ui_tests CONSOLE UI
        SOURCES     tests/ui_tests.cpp
        DESCRIPTION "libdesktop-ui unit tests"
    )

    add_test(NAME libdesktop-ui COMMAND libdesktop_ui_tests)
    set_tests_properties(libdesktop-ui PROPERTIES LABELS libdesktop-ui)
endif()
