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

# libdesktop's own targets build without warnings at /W4.
function(libdesktop_enable_msvc_warnings target)
    if(MSVC)
        target_compile_options(${target} PRIVATE /W4)
    endif()
endfunction()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/core.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/platform.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/render.cmake")
include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/ui.cmake")

if(NOT PROJECT_IS_TOP_LEVEL)
    set(libdesktop_targets libdesktop-core libdesktop-platform libdesktop-render libdesktop-ui)
    foreach(target IN LISTS libdesktop_targets)
        set_target_properties(${target} PROPERTIES EXCLUDE_FROM_ALL TRUE)
    endforeach()
endif()

include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/libdesktop_add_app.cmake")

if(PROJECT_IS_TOP_LEVEL)
    enable_testing()

    add_executable(libdesktop_core_tests
        tests/core/system_tests.cpp tests/core/render_surface_tests.cpp tests/core/mcp_tests.cpp tests/test.h)
    target_link_libraries(libdesktop_core_tests PRIVATE libdesktop::core)
    target_include_directories(libdesktop_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_precompile_headers(libdesktop_core_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/core.h")
    if(MSVC)
        target_compile_options(libdesktop_core_tests PRIVATE /bigobj /W4)
    else()
        target_compile_options(libdesktop_core_tests PRIVATE -Wall -Wno-unknown-pragmas)
    endif()
    libdesktop_enable_msvc_exceptions(libdesktop_core_tests)

    add_test(NAME libdesktop-core COMMAND libdesktop_core_tests)
    set_tests_properties(libdesktop-core PROPERTIES LABELS "libdesktop-core;headless")

    add_executable(libdesktop_platform_tests
        tests/platform/platform_tests.cpp tests/platform/system_tests.cpp
        tests/platform/platform_tasks_tests.cpp tests/platform/platform_media_tests.cpp
        tests/platform/platform_services_tests.cpp tests/platform/platform_shell_tests.cpp
        tests/platform/test_support.h tests/test.h)
    target_link_libraries(libdesktop_platform_tests PRIVATE libdesktop::platform)
    target_include_directories(libdesktop_platform_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_precompile_headers(libdesktop_platform_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/platform.h")
    if(WIN32)
        # The loopback server is Winsock; the web client tests that use it are Windows-only.
        target_sources(libdesktop_platform_tests PRIVATE
            tests/platform/win/system_tests.cpp tests/platform/win/render_tests.cpp
            tests/platform/win/os_tests.cpp tests/platform/loopback_http.cpp tests/platform/loopback_http.h
            tests/platform/win/instance_tests.cpp tests/platform/win/prompt_tests.cpp
            tests/tests.manifest)
        target_include_directories(libdesktop_platform_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win")
        target_compile_definitions(libdesktop_platform_tests PRIVATE UNICODE _UNICODE)
    else()
        target_sources(libdesktop_platform_tests PRIVATE tests/platform/linux/window_icon_tests.cpp)
        target_include_directories(libdesktop_platform_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/linux")
        target_link_libraries(libdesktop_platform_tests PRIVATE SDL3::SDL3-static)
    endif()
    if(MSVC)
        target_compile_options(libdesktop_platform_tests PRIVATE /bigobj /W4)
    else()
        target_compile_options(libdesktop_platform_tests PRIVATE -Wall -Wno-unknown-pragmas)
    endif()
    libdesktop_enable_msvc_exceptions(libdesktop_platform_tests)

    add_test(NAME libdesktop-platform COMMAND libdesktop_platform_tests)
    set_tests_properties(libdesktop-platform PROPERTIES
        LABELS "libdesktop-platform;headless" RESOURCE_LOCK libdesktop-clipboard)

    find_package(Python3 3.9 QUIET COMPONENTS Interpreter)
    if(Python3_Interpreter_FOUND)
        add_test(NAME libdesktop-icon-tools COMMAND "${Python3_EXECUTABLE}"
            "${CMAKE_CURRENT_SOURCE_DIR}/tools/test_iconlib.py")
        set_tests_properties(libdesktop-icon-tools PROPERTIES LABELS "libdesktop-platform;headless" TIMEOUT 30)
    endif()

    add_executable(libdesktop_render_tests
        tests/render/render_tests.cpp tests/render/render_primitives_tests.cpp tests/render/render_polygon_tests.cpp
        tests/test.h)
    target_link_libraries(libdesktop_render_tests PRIVATE libdesktop::render)
    target_include_directories(libdesktop_render_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_precompile_headers(libdesktop_render_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/render.h")
    if(WIN32)
        target_compile_definitions(libdesktop_render_tests PRIVATE UNICODE _UNICODE)
        target_sources(libdesktop_render_tests PRIVATE tests/render/win/render_win_tests.cpp)
    endif()
    if(MSVC)
        target_compile_options(libdesktop_render_tests PRIVATE /bigobj /W4)
    else()
        target_compile_options(libdesktop_render_tests PRIVATE -Wall -Wno-unknown-pragmas)
    endif()
    libdesktop_enable_msvc_exceptions(libdesktop_render_tests)

    add_test(NAME libdesktop-render COMMAND libdesktop_render_tests)
    set_tests_properties(libdesktop-render PROPERTIES LABELS "libdesktop-render;headless")

    add_executable(libdesktop_ui_tests
        tests/ui/ui_tests.cpp tests/ui/render_tests.cpp tests/ui/render_primitives_tests.cpp
        tests/ui/render_controls_tests.cpp tests/ui/render_elements_tests.cpp
        tests/ui/ui_menu_bar_tests.cpp tests/ui/ui_text_tests.cpp tests/ui/ui_view_tests.cpp
        tests/ui/ui_list_view_tests.cpp tests/ui/ui_doc_view_tests.cpp tests/ui/ui_markdown_view_tests.cpp
        tests/ui/text_tests.cpp tests/ui/ui_shell_tests.cpp tests/test.h)
    target_link_libraries(libdesktop_ui_tests PRIVATE libdesktop::ui)
    target_include_directories(libdesktop_ui_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_precompile_headers(libdesktop_ui_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/ui.h")
    if(WIN32)
        target_compile_definitions(libdesktop_ui_tests PRIVATE UNICODE _UNICODE)
        target_sources(libdesktop_ui_tests PRIVATE tests/ui/win/render_tests.cpp tests/tests.manifest)
    endif()
    if(MSVC)
        target_compile_options(libdesktop_ui_tests PRIVATE /bigobj /W4)
    else()
        target_compile_options(libdesktop_ui_tests PRIVATE -Wall -Wno-unknown-pragmas)
    endif()
    libdesktop_enable_msvc_exceptions(libdesktop_ui_tests)

    add_test(NAME libdesktop-ui COMMAND libdesktop_ui_tests)
    set_tests_properties(libdesktop-ui PROPERTIES
        LABELS "libdesktop-ui;headless" RESOURCE_LOCK libdesktop-clipboard)

    # Exercise the public libdesktop_add_app() helper, the platform resource registry and the event loop.
    libdesktop_add_app(libdesktop_platform_smoke
        SOURCES tests/platform_registry_smoke.cpp
        DESCRIPTION "libdesktop platform registry smoke"
        EMBED tests/data/sample.css
    )
    libdesktop_enable_msvc_exceptions(libdesktop_platform_smoke)
    libdesktop_enable_msvc_warnings(libdesktop_platform_smoke)
    add_test(NAME libdesktop-platform-folder-watch COMMAND libdesktop_platform_smoke --check-folder-watch)
    set_tests_properties(libdesktop-platform-folder-watch PROPERTIES LABELS "libdesktop-platform;headless" TIMEOUT 30)
    if(NOT WIN32)
        # The event loop opens a display, and SDL's offscreen driver is one without a screen.
        set_tests_properties(libdesktop-platform-folder-watch PROPERTIES ENVIRONMENT "SDL_VIDEODRIVER=offscreen")
    endif()

    # A copy of the smoke serves an agent session over real pipes, with no window and, on Linux, no display needed.
    add_test(NAME libdesktop-platform-agent COMMAND libdesktop_platform_smoke --check-agent)
    set_tests_properties(libdesktop-platform-agent PROPERTIES LABELS "libdesktop-platform;headless" TIMEOUT 60)

    if(WIN32)
        add_test(NAME libdesktop-platform-smoke COMMAND libdesktop_platform_smoke --check-embedded)
        set_tests_properties(libdesktop-platform-smoke PROPERTIES LABELS "libdesktop-platform;headless")
        add_test(NAME libdesktop-platform-ui-pump COMMAND libdesktop_platform_smoke --check-ui-pump)
        set_tests_properties(libdesktop-platform-ui-pump PROPERTIES LABELS "libdesktop-platform;headless" TIMEOUT 30)
        add_test(NAME libdesktop-platform-busy-input COMMAND libdesktop_platform_smoke --check-busy-input)
        set_tests_properties(libdesktop-platform-busy-input PROPERTIES LABELS "libdesktop-platform;headless" TIMEOUT 30)

        # Makes real frames over an offscreen application window, none of them shown.
        libdesktop_add_app(libdesktop_ui_smoke
            SOURCES tests/ui/frame_creation_smoke.cpp
            DESCRIPTION "libdesktop frame creation smoke"
        )
        target_link_libraries(libdesktop_ui_smoke PRIVATE libdesktop::ui)
        libdesktop_enable_msvc_exceptions(libdesktop_ui_smoke)
        libdesktop_enable_msvc_warnings(libdesktop_ui_smoke)
        add_test(NAME libdesktop-ui-frame-creation COMMAND libdesktop_ui_smoke --check-frame-creation)
        set_tests_properties(libdesktop-ui-frame-creation PROPERTIES LABELS "libdesktop-ui;headless" TIMEOUT 30)

        # Runs a shell application - its menu bar, fixed-step loop, keys and accelerators - over an offscreen window.
        libdesktop_add_app(libdesktop_ui_shell_smoke
            SOURCES tests/ui/shell_smoke.cpp
            DESCRIPTION "libdesktop shell smoke"
        )
        target_link_libraries(libdesktop_ui_shell_smoke PRIVATE libdesktop::ui)
        libdesktop_enable_msvc_exceptions(libdesktop_ui_shell_smoke)
        libdesktop_enable_msvc_warnings(libdesktop_ui_shell_smoke)
        add_test(NAME libdesktop-ui-shell COMMAND libdesktop_ui_shell_smoke)
        set_tests_properties(libdesktop-ui-shell PROPERTIES LABELS "libdesktop-ui;headless" TIMEOUT 30)
    endif()
endif()
