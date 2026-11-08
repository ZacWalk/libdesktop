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
        tests/core/system_tests.cpp tests/core/render_surface_tests.cpp tests/test.h)
    target_link_libraries(libdesktop_core_tests PRIVATE libdesktop::core)
    target_include_directories(libdesktop_core_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_precompile_headers(libdesktop_core_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/core.h")
    if(MSVC)
        target_compile_options(libdesktop_core_tests PRIVATE /bigobj /W3)
    else()
        target_compile_options(libdesktop_core_tests PRIVATE -Wall -Wno-unknown-pragmas)
    endif()
    libdesktop_enable_msvc_exceptions(libdesktop_core_tests)

    add_test(NAME libdesktop-core COMMAND libdesktop_core_tests)
    set_tests_properties(libdesktop-core PROPERTIES LABELS libdesktop-core)

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
            tests/tests.manifest)
        target_include_directories(libdesktop_platform_tests PRIVATE
            "${CMAKE_CURRENT_SOURCE_DIR}/src/platform/win")
        target_compile_definitions(libdesktop_platform_tests PRIVATE UNICODE _UNICODE)
    endif()
    if(MSVC)
        target_compile_options(libdesktop_platform_tests PRIVATE /bigobj /W3)
    else()
        target_compile_options(libdesktop_platform_tests PRIVATE -Wall -Wno-unknown-pragmas)
    endif()
    libdesktop_enable_msvc_exceptions(libdesktop_platform_tests)

    add_test(NAME libdesktop-platform COMMAND libdesktop_platform_tests)
    set_tests_properties(libdesktop-platform PROPERTIES
        LABELS libdesktop-platform RESOURCE_LOCK libdesktop-clipboard)

    add_executable(libdesktop_render_tests
        tests/render/render_tests.cpp tests/render/render_primitives_tests.cpp tests/test.h)
    target_link_libraries(libdesktop_render_tests PRIVATE libdesktop::render)
    target_include_directories(libdesktop_render_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_precompile_headers(libdesktop_render_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/render.h")
    if(WIN32)
        target_compile_definitions(libdesktop_render_tests PRIVATE UNICODE _UNICODE)
        target_sources(libdesktop_render_tests PRIVATE tests/render/win/render_win_tests.cpp)
    endif()
    if(MSVC)
        target_compile_options(libdesktop_render_tests PRIVATE /bigobj /W3)
    else()
        target_compile_options(libdesktop_render_tests PRIVATE -Wall -Wno-unknown-pragmas)
    endif()
    libdesktop_enable_msvc_exceptions(libdesktop_render_tests)

    add_test(NAME libdesktop-render COMMAND libdesktop_render_tests)
    set_tests_properties(libdesktop-render PROPERTIES LABELS libdesktop-render)

    add_executable(libdesktop_ui_tests
        tests/ui/ui_tests.cpp tests/ui/render_tests.cpp tests/ui/render_primitives_tests.cpp
        tests/ui/render_controls_tests.cpp tests/ui/render_elements_tests.cpp
        tests/ui/ui_menu_bar_tests.cpp tests/ui/ui_text_tests.cpp tests/ui/ui_view_tests.cpp
        tests/ui/ui_list_view_tests.cpp tests/ui/ui_doc_view_tests.cpp tests/ui/ui_markdown_view_tests.cpp
        tests/ui/text_tests.cpp tests/test.h)
    target_link_libraries(libdesktop_ui_tests PRIVATE libdesktop::ui)
    target_include_directories(libdesktop_ui_tests PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/tests")
    target_precompile_headers(libdesktop_ui_tests PRIVATE
        "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/ui.h")
    if(WIN32)
        target_compile_definitions(libdesktop_ui_tests PRIVATE UNICODE _UNICODE)
        target_sources(libdesktop_ui_tests PRIVATE tests/ui/win/render_tests.cpp tests/tests.manifest)
    endif()
    if(MSVC)
        target_compile_options(libdesktop_ui_tests PRIVATE /bigobj /W3)
    else()
        target_compile_options(libdesktop_ui_tests PRIVATE -Wall -Wno-unknown-pragmas)
    endif()
    libdesktop_enable_msvc_exceptions(libdesktop_ui_tests)

    add_test(NAME libdesktop-ui COMMAND libdesktop_ui_tests)
    set_tests_properties(libdesktop-ui PROPERTIES
        LABELS libdesktop-ui RESOURCE_LOCK libdesktop-clipboard)

    if(WIN32)
        # Exercise the public libdesktop_add_app() helper and the platform resource registry.
        libdesktop_add_app(libdesktop_platform_smoke
            SOURCES tests/platform_registry_smoke.cpp
            DESCRIPTION "libdesktop platform registry smoke"
            EMBED tests/data/sample.css
        )
        libdesktop_enable_msvc_exceptions(libdesktop_platform_smoke)
        add_test(NAME libdesktop-platform-smoke COMMAND libdesktop_platform_smoke --check-embedded)
        set_tests_properties(libdesktop-platform-smoke PROPERTIES LABELS libdesktop-platform)
        add_test(NAME libdesktop-platform-ui-pump COMMAND libdesktop_platform_smoke --check-ui-pump)
        set_tests_properties(libdesktop-platform-ui-pump PROPERTIES LABELS libdesktop-platform TIMEOUT 30)

        # Makes real frames over an offscreen application window, none of them shown.
        libdesktop_add_app(libdesktop_ui_smoke
            SOURCES tests/ui/frame_creation_smoke.cpp
            DESCRIPTION "libdesktop frame creation smoke"
        )
        target_link_libraries(libdesktop_ui_smoke PRIVATE libdesktop::ui)
        libdesktop_enable_msvc_exceptions(libdesktop_ui_smoke)
        add_test(NAME libdesktop-ui-frame-creation COMMAND libdesktop_ui_smoke --check-frame-creation)
        set_tests_properties(libdesktop-ui-frame-creation PROPERTIES LABELS libdesktop-ui TIMEOUT 30)
    endif()
endif()
