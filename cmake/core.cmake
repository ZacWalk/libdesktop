# libdesktop-core: portable C++20 types, utilities, pixels and image primitives.

include("${CMAKE_CURRENT_SOURCE_DIR}/.dd/dependencies.cmake")

dd_dependency_arguments("${CMAKE_CURRENT_SOURCE_DIR}/cmake/dd-dependencies.json" rapidjson
    libdesktop_rapidjson_acquisition)
FetchContent_Declare(rapidjson ${libdesktop_rapidjson_acquisition} SOURCE_SUBDIR "no-cmake-project")
FetchContent_MakeAvailable(rapidjson)

set(libdesktop_core_headers
    include/libdesktop/core.h
    include/libdesktop/core/base64.h
    include/libdesktop/core/cpu_features.h
    include/libdesktop/core/date.h
    include/libdesktop/core/dialect.h
    include/libdesktop/core/geometry.h
    include/libdesktop/core/json.h
    include/libdesktop/core/kdtree.h
    include/libdesktop/core/log.h
    include/libdesktop/core/map.h
    include/libdesktop/core/path.h
    include/libdesktop/core/pixels.h
    include/libdesktop/core/primitives.h
    include/libdesktop/core/selector.h
    include/libdesktop/core/simd.h
    include/libdesktop/core/strings.h
    include/libdesktop/core/util.h
)

set(libdesktop_core_sources
    ${libdesktop_core_headers}
    src/core/util.cpp
    src/core/strings.cpp
    src/core/geometry.cpp
    src/core/base64.cpp
    src/core/json.cpp
    src/core/surface.cpp
    src/core/image.cpp
    src/core/pixel_blend.cpp
)

# Keep the baseline translation unit free of AVX instructions. The wider kernels are compiled in
# their own unit and selected only after the runtime feature query confirms OS support.
set(libdesktop_core_x86 FALSE)
if(CMAKE_GENERATOR_PLATFORM MATCHES "^(x64|Win32)$")
    set(libdesktop_core_x86 TRUE)
elseif(NOT CMAKE_GENERATOR_PLATFORM AND MSVC)
    get_filename_component(libdesktop_core_compiler_bin "${CMAKE_CXX_COMPILER}" DIRECTORY)
    if(libdesktop_core_compiler_bin MATCHES "[/\\](x64|x86)$")
        set(libdesktop_core_x86 TRUE)
    endif()
elseif(CMAKE_SYSTEM_PROCESSOR MATCHES "^(x86_64|AMD64|amd64|i[3-6]86|x86)$")
    set(libdesktop_core_x86 TRUE)
endif()
if(libdesktop_core_x86)
    list(APPEND libdesktop_core_sources src/core/pixel_blend_avx2.cpp)
endif()

if(WIN32)
    list(APPEND libdesktop_core_sources src/core/win/primitives.cpp)
else()
    list(APPEND libdesktop_core_sources src/core/linux/primitives.cpp)
endif()

add_library(libdesktop-core STATIC ${libdesktop_core_sources})
add_library(libdesktop::core ALIAS libdesktop-core)

target_compile_features(libdesktop-core PUBLIC cxx_std_20)
target_include_directories(libdesktop-core PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_include_directories(libdesktop-core SYSTEM PUBLIC "${rapidjson_SOURCE_DIR}/include")
target_precompile_headers(libdesktop-core PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/core.h")

if(MSVC)
    target_compile_options(libdesktop-core PUBLIC /utf-8 PRIVATE /bigobj /W3 /sdl)
    libdesktop_enable_msvc_exceptions(libdesktop-core)
    if(libdesktop_core_x86)
        set_source_files_properties(src/core/pixel_blend_avx2.cpp PROPERTIES COMPILE_OPTIONS /arch:AVX2)
        target_compile_definitions(libdesktop-core PRIVATE LIBDESKTOP_PIXEL_AVX2_TU)
    endif()
elseif(CMAKE_CXX_COMPILER_ID MATCHES "^(GNU|Clang)$")
    target_compile_options(libdesktop-core PRIVATE -Wall -Wno-unknown-pragmas -Wno-multichar)
    if(libdesktop_core_x86)
        # GCC rejects a precompiled header built without -mavx2, so this one file compiles without it.
        set_source_files_properties(src/core/pixel_blend_avx2.cpp PROPERTIES COMPILE_OPTIONS -mavx2
            SKIP_PRECOMPILE_HEADERS ON)
        target_compile_definitions(libdesktop-core PRIVATE LIBDESKTOP_PIXEL_AVX2_TU)
    endif()
endif()

if(WIN32)
    target_compile_definitions(libdesktop-core PRIVATE UNICODE _UNICODE)
else()
    find_package(Threads REQUIRED)
    target_link_libraries(libdesktop-core PUBLIC Threads::Threads)
endif()
