# libdesktop-system: the foundation (df::, str::) and the OS services (platform::), for Windows and
# Linux. CMakeLists.txt includes this when LIBDESKTOP_SYSTEM is on; docs/system.md describes the module.

include("${CMAKE_CURRENT_SOURCE_DIR}/.dd/dependencies.cmake")

# RapidJSON is header-only, so it is fetched but not configured: its own CMakeLists builds documents
# and tests. A consumer that carries the same revision can point FETCHCONTENT_SOURCE_DIR_RAPIDJSON at
# it, so that one program holds one copy.
dd_dependency_arguments("${CMAKE_CURRENT_SOURCE_DIR}/cmake/dd-dependencies.json" rapidjson
    libdesktop_rapidjson_acquisition)
FetchContent_Declare(rapidjson ${libdesktop_rapidjson_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(rapidjson)

add_library(libdesktop-system STATIC
    include/libdesktop/system.h
    include/libdesktop/platform_compat.h
    include/libdesktop/platform.h
    include/libdesktop/platform_environment.h
    include/libdesktop/util.h
    include/libdesktop/util_base64.h
    include/libdesktop/util_date.h
    include/libdesktop/util_file.h
    include/libdesktop/util_geometry.h
    include/libdesktop/util_json.h
    include/libdesktop/util_kdtree.h
    include/libdesktop/util_log.h
    include/libdesktop/util_map.h
    include/libdesktop/util_path.h
    include/libdesktop/util_selector.h
    include/libdesktop/util_simd.h
    include/libdesktop/util_strings.h
    src/system/util.cpp
    src/system/util_base64.cpp
    src/system/util_geometry.cpp
    src/system/util_json.cpp
    src/system/util_strings.cpp
)
add_library(libdesktop::system ALIAS libdesktop-system)

target_compile_features(libdesktop-system PUBLIC cxx_std_20)

# Reached as #include "libdesktop/util.h", so the names cannot collide with an application's own
# headers, or with libdesktop-core's platform.h.
target_include_directories(libdesktop-system PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(libdesktop-system SYSTEM PUBLIC "${rapidjson_SOURCE_DIR}/include")
target_precompile_headers(libdesktop-system PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/system.h")

if(MSVC)
    # /utf-8 is public because the headers hold UTF-8 literals that a consumer compiles too.
    target_compile_options(libdesktop-system PUBLIC /utf-8 PRIVATE /bigobj /W3 /sdl)
else()
    target_compile_options(libdesktop-system PRIVATE -Wall -Wno-unknown-pragmas -Wno-multichar)
endif()

if(WIN32)
    target_sources(libdesktop-system PRIVATE
        include/libdesktop/platform_win.h
        include/libdesktop/platform_win_process.h
        src/system/platform_win.cpp
        src/system/platform_win_files.cpp
        src/system/platform_win_process.cpp
        src/system/platform_win_settings.cpp
        src/system/platform_win_share.cpp
        src/system/platform_win_web.cpp
    )

    target_compile_definitions(libdesktop-system PUBLIC UNICODE _UNICODE)

    # A Store package is updated by the Store, so the application-owned update and install path is
    # left out of the API as well as the backend.
    option(LIBDESKTOP_STORE_PACKAGE "Build libdesktop-system for a Store package (defines WINSTORE)" OFF)
    if(LIBDESKTOP_STORE_PACKAGE)
        target_compile_definitions(libdesktop-system PUBLIC WINSTORE)
    endif()

    # The sources also name their libraries with #pragma comment(lib); these are what a link needs
    # besides, measured by linking the module's objects alone.
    target_link_libraries(libdesktop-system PUBLIC user32 gdi32 shell32 comdlg32 ole32 oleaut32)
else()
    target_sources(libdesktop-system PRIVATE
        src/system/platform_linux.cpp
        src/system/platform_linux_desktop.cpp
        src/system/platform_linux_files.cpp
        src/system/platform_linux_settings.cpp
    )

    find_package(Threads REQUIRED)
    target_link_libraries(libdesktop-system PUBLIC Threads::Threads)
endif()
