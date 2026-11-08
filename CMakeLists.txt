cmake_minimum_required(VERSION 3.21)

project(libdesktop LANGUAGES CXX VERSION 0.1.0)

function(libdesktop_enable_msvc_exceptions target)
    if(MSVC)
        string(FIND "${CMAKE_CXX_FLAGS}" "/EHsc" libdesktop_ehsc_pos)
        if(libdesktop_ehsc_pos EQUAL -1)
            target_compile_options(${target} PRIVATE /EHsc)
        endif()
    endif()
endfunction()

# --- libdesktop-os: the OS services libdesktop-core and libdesktop-system share ---
# A service here names no application and writes no log, so both stacks call it: libdesktop-core's
# backend for pf::, libdesktop-system's Windows backend for platform:: (docs/system.md). Its backend
# is Windows; libdesktop-system's Linux backend still carries its own implementations.
if(WIN32)
    add_library(libdesktop-os STATIC
        include/libdesktop/os.h
        src/os/os_win.cpp
        src/os/os_win_audio.cpp
        src/os/os_win_text.h
    )
    add_library(libdesktop::os ALIAS libdesktop-os)

    target_compile_features(libdesktop-os PUBLIC cxx_std_20)
    target_include_directories(libdesktop-os PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
    target_compile_definitions(libdesktop-os PRIVATE UNICODE _UNICODE)
    if(MSVC)
        target_compile_options(libdesktop-os PRIVATE /W3)
    endif()
    libdesktop_enable_msvc_exceptions(libdesktop-os)
    target_link_libraries(libdesktop-os PUBLIC wininet winhttp user32 shell32 ole32 avrt)

    if(PROJECT_IS_TOP_LEVEL)
        enable_testing()

        add_executable(libdesktop_os_tests tests/os_tests.cpp tests/check.h tests/loopback_http.cpp
            tests/loopback_http.h)
        target_link_libraries(libdesktop_os_tests PRIVATE libdesktop::os)
        if(MSVC)
            target_compile_options(libdesktop_os_tests PRIVATE /W3)
        endif()
        libdesktop_enable_msvc_exceptions(libdesktop_os_tests)

        add_test(NAME libdesktop-os COMMAND libdesktop_os_tests)
        set_tests_properties(libdesktop-os PROPERTIES
            LABELS libdesktop-os RESOURCE_LOCK libdesktop-clipboard)
    endif()
endif()

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
            target_sources(libdesktop_system_tests PRIVATE tests/system_win_tests.cpp tests/system_tests.manifest
                tests/loopback_http.cpp tests/loopback_http.h)
        endif()
        target_link_libraries(libdesktop_system_tests PRIVATE libdesktop::system)
        target_precompile_headers(libdesktop_system_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/system.h")
        if(MSVC)
            target_compile_options(libdesktop_system_tests PRIVATE /bigobj /W3)
        else()
            target_compile_options(libdesktop_system_tests PRIVATE -Wall -Wno-unknown-pragmas)
        endif()
        libdesktop_enable_msvc_exceptions(libdesktop_system_tests)

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
        add_executable(libdesktop_render_tests tests/render_tests.cpp tests/render_controls_tests.cpp
            tests/render_elements_tests.cpp tests/system_test.h)
        if(WIN32)
            target_compile_definitions(libdesktop_render_tests PRIVATE UNICODE _UNICODE)
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
        libdesktop_enable_msvc_exceptions(libdesktop_render_tests)

        add_test(NAME libdesktop-render COMMAND libdesktop_render_tests)
        set_tests_properties(libdesktop-render PROPERTIES LABELS libdesktop-render)
    endif()
endif()

# --- libdesktop-text: the text layer's vocabulary and its syntax highlighters ----
# Standard C++ alone, so it builds wherever libdesktop does. libdesktop-ui is written in it, and an
# application on another window model links it alone to colour text it draws itself.
add_library(libdesktop-text STATIC
    src/ui/text_types.h
    src/ui/syntax.h
    src/ui/syntax.cpp
)
add_library(libdesktop::text ALIAS libdesktop-text)

target_compile_features(libdesktop-text PUBLIC cxx_std_20)

# Reached as #include "ui/syntax.h", the spelling libdesktop-ui and its consumers already use.
target_include_directories(libdesktop-text PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")
libdesktop_enable_msvc_exceptions(libdesktop-text)

if(PROJECT_IS_TOP_LEVEL)
    enable_testing()

    add_executable(libdesktop_text_tests tests/text_tests.cpp tests/check.h)
    target_link_libraries(libdesktop_text_tests PRIVATE libdesktop::text)
    if(MSVC)
        target_compile_options(libdesktop_text_tests PRIVATE /W3)
    else()
        target_compile_options(libdesktop_text_tests PRIVATE -Wall -Wno-unknown-pragmas)
    endif()
    libdesktop_enable_msvc_exceptions(libdesktop_text_tests)

    add_test(NAME libdesktop-text COMMAND libdesktop_text_tests)
    set_tests_properties(libdesktop-text PROPERTIES LABELS libdesktop-text)
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
    src/core/platform_pixel.cpp
)
add_library(libdesktop::core ALIAS libdesktop-core)

target_compile_features(libdesktop-core PUBLIC cxx_std_20)
if(MSVC)
    libdesktop_enable_msvc_exceptions(libdesktop-core)
endif()

# Keeps #include "platform.h" resolving for every consumer after the move.
target_include_directories(libdesktop-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src/core")

if(WIN32)
    if(NOT MSVC)
        message(FATAL_ERROR "The Win32 backend currently requires MSVC.")
    endif()

    enable_language(RC)

    target_sources(libdesktop-core PRIVATE src/core/platform_win.cpp src/core/platform_win_image.cpp
        src/core/platform_win_compound.cpp src/core/platform_audio.h src/core/platform_audio.cpp
        src/core/platform_win_interop.h)

    # Keep the AVX2 code in its own object: the rest of Core must retain its
    # runtime-dispatch baseline and remain buildable for non-x86 targets.
    set(libdesktop_x86 FALSE)
    if(CMAKE_GENERATOR_PLATFORM MATCHES "^(x64|Win32)$")
        set(libdesktop_x86 TRUE)
    elseif(NOT CMAKE_GENERATOR_PLATFORM)
        # Ninja does not expose a platform name. The MSVC compiler path still
        # ends in x64/x86 (and in arm64 for the scalar-only build).
        get_filename_component(libdesktop_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
        if(libdesktop_compiler_bin MATCHES "[/\\\\](x64|x86)$")
            set(libdesktop_x86 TRUE)
        endif()
    endif()
    if(libdesktop_x86)
        target_sources(libdesktop-core PRIVATE src/core/platform_pixel_avx2.cpp)
        set_source_files_properties(src/core/platform_pixel_avx2.cpp
            PROPERTIES COMPILE_OPTIONS /arch:AVX2)
        target_compile_definitions(libdesktop-core PRIVATE PF_PIXEL_AVX2_TU)
    endif()

    # WIN32_LEAN_AND_MEAN / NOMINMAX are defined by platform_win.cpp itself;
    # defining them here as well would be a macro redefinition.
    target_compile_definitions(libdesktop-core PUBLIC WIN32 _WINDOWS)
    target_compile_definitions(libdesktop-core PRIVATE UNICODE _UNICODE)

    # The services pf:: shares with libdesktop-system, its audio endpoint among them. Private: an
    # application links it through libdesktop-core and never includes it.
    target_link_libraries(libdesktop-core PRIVATE libdesktop::os)

    # platform_win.cpp still carries #pragma comment(lib, ...) for these; naming
    # them here is what a non-MSVC backend will rely on once the pragmas go.
    target_link_libraries(libdesktop-core PUBLIC
        shell32 comdlg32 shlwapi windowscodecs winhttp comctl32
        ole32 oleaut32 user32 gdi32 advapi32 msimg32
    )
else()
    message(FATAL_ERROR "libdesktop has a Win32 backend only; add src for this platform first.")
endif()

# --- libdesktop-ui: reusable widgets, text views and the chat panel ----------
# Opt in with platform_add_app(... UI). An app that does not ask for it does not
# link it, so the games and viewers keep their current size.
add_library(libdesktop-ui STATIC
    src/ui/ui.h
    src/ui/theme.h
    src/ui/theme.cpp
    src/ui/view_host.h
    src/ui/widgets.h
    src/ui/table_layout.h
    src/ui/table_layout.cpp
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

target_link_libraries(libdesktop-ui PUBLIC libdesktop::core libdesktop::text)
if(MSVC)
    libdesktop_enable_msvc_exceptions(libdesktop-ui)
endif()

# Reached as #include "ui/widgets.h", so generic names like widgets.h or theme.h
# cannot collide with an application's own headers.
target_include_directories(libdesktop-ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform_app.cmake")

# Only when built directly; apps that pull this in with FetchContent do not want
# the test target in their build.
if(PROJECT_IS_TOP_LEVEL)
    enable_testing()

    platform_add_app(libdesktop_tests CONSOLE
        SOURCES     tests/platform_tests.cpp tests/loopback_http.cpp tests/loopback_http.h
        DESCRIPTION "libdesktop unit tests"
        EMBED       tests/data/sample.css
    )
    libdesktop_enable_msvc_exceptions(libdesktop_tests)
    target_compile_definitions(libdesktop_tests PRIVATE UNICODE _UNICODE)

    add_test(NAME libdesktop COMMAND libdesktop_tests)
    set_tests_properties(libdesktop PROPERTIES LABELS libdesktop)

    platform_add_app(libdesktop_ui_tests CONSOLE UI
        SOURCES     tests/ui_tests.cpp
        DESCRIPTION "libdesktop-ui unit tests"
    )
    libdesktop_enable_msvc_exceptions(libdesktop_ui_tests)
    target_compile_definitions(libdesktop_ui_tests PRIVATE UNICODE _UNICODE)

    add_test(NAME libdesktop-ui COMMAND libdesktop_ui_tests)
    set_tests_properties(libdesktop-ui PROPERTIES
        LABELS libdesktop-ui RESOURCE_LOCK libdesktop-clipboard)
endif()
