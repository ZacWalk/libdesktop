# libdesktop-render: rendering contexts, text, probes, software and native backends.

set(libdesktop_render_headers
    include/libdesktop/render.h
    include/libdesktop/render/context.h
    include/libdesktop/render/counters.h
    include/libdesktop/render/probes.h
    include/libdesktop/render/device.h
    include/libdesktop/render/software.h
    include/libdesktop/render/text.h
    include/libdesktop/render/win32.h
)

add_library(libdesktop-render STATIC
    ${libdesktop_render_headers}
    src/render/color.cpp
    src/render/font.cpp
    src/render/software.cpp
    src/render/software_context.cpp
    src/render/device.cpp
    src/render/shadow.cpp
)
add_library(libdesktop::render ALIAS libdesktop-render)

target_compile_features(libdesktop-render PUBLIC cxx_std_20)
target_link_libraries(libdesktop-render PUBLIC libdesktop::platform)
target_include_directories(libdesktop-render PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_precompile_headers(libdesktop-render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/render.h")

if(MSVC)
    target_compile_options(libdesktop-render PRIVATE /bigobj /W3 /sdl)
    libdesktop_enable_msvc_exceptions(libdesktop-render)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(libdesktop-render PRIVATE -Wall -Wno-unknown-pragmas -Wno-multichar)
endif()

set(libdesktop_render_embedded "${CMAKE_CURRENT_BINARY_DIR}/libdesktop_render_embedded.cpp")
set(libdesktop_render_embedded_text "// Generated from src/render/res by CMake. Do not edit.\n#include <cstddef>\n")
set(libdesktop_shadow_path "${CMAKE_CURRENT_SOURCE_DIR}/src/render/res/shadow.png")
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${libdesktop_shadow_path}")
file(READ "${libdesktop_shadow_path}" libdesktop_shadow_hex HEX)
string(REGEX REPLACE "([0-9A-Fa-f][0-9A-Fa-f])" "0x\\1," libdesktop_shadow_bytes "${libdesktop_shadow_hex}")
string(LENGTH "${libdesktop_shadow_hex}" libdesktop_shadow_hex_length)
math(EXPR libdesktop_shadow_length "${libdesktop_shadow_hex_length} / 2")
string(APPEND libdesktop_render_embedded_text
    "extern const unsigned char libdesktop_shadow_png[] = {${libdesktop_shadow_bytes}};\n"
    "extern const std::size_t libdesktop_shadow_png_size = ${libdesktop_shadow_length};\n")
file(WRITE "${libdesktop_render_embedded}.in" "${libdesktop_render_embedded_text}")
configure_file("${libdesktop_render_embedded}.in" "${libdesktop_render_embedded}" COPYONLY)
set_source_files_properties("${libdesktop_render_embedded}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
target_sources(libdesktop-render PRIVATE "${libdesktop_render_embedded}")

if(WIN32)
    # Consumers still compile their own executable resource scripts; enable RC once for the tree.
    enable_language(RC)
    target_compile_definitions(libdesktop-render PRIVATE UNICODE _UNICODE)
    target_sources(libdesktop-render PRIVATE
        include/libdesktop/render/win32.h
        src/render/win/d3d11.cpp
        src/render/win/font.cpp
        src/render/win/software_present.cpp
        src/render/win/text_surfaces.cpp
        src/render/win/window_device.cpp
    )

    file(GLOB libdesktop_fxc_candidates
        "$ENV{ProgramFiles\(x86\)}/Windows Kits/10/bin/*/x64/fxc.exe"
        "$ENV{ProgramFiles}/Windows Kits/10/bin/*/x64/fxc.exe")
    set(libdesktop_fxc_dirs "")
    foreach(candidate IN LISTS libdesktop_fxc_candidates)
        get_filename_component(dir "${candidate}" DIRECTORY)
        list(APPEND libdesktop_fxc_dirs "${dir}")
    endforeach()
    list(SORT libdesktop_fxc_dirs)
    list(REVERSE libdesktop_fxc_dirs)

    find_program(FXC_EXECUTABLE NAMES fxc HINTS ${libdesktop_fxc_dirs} DOC "The Direct3D shader compiler")
    if(NOT FXC_EXECUTABLE)
        message(FATAL_ERROR "fxc was not found; libdesktop-render's shaders cannot be built")
    endif()

    set(libdesktop_render_shader_dir "${CMAKE_CURRENT_BINARY_DIR}/render_shaders")
    file(MAKE_DIRECTORY "${libdesktop_render_shader_dir}")

    set(libdesktop_pixel_shaders circle_ps font_ps pano_ps rgb_bicubic_ps rgb_ps solid_ps yuv_bicubic_ps yuv_ps
        yuv_hdr_bicubic_ps yuv_hdr_ps)
    set(libdesktop_vertex_shaders vs)
    set(libdesktop_render_shader_outputs "")
    set(libdesktop_render_shader_symbols "")
    file(GLOB libdesktop_shader_includes CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/*.hlsli")

    foreach(shader IN LISTS libdesktop_pixel_shaders libdesktop_vertex_shaders)
        if(shader IN_LIST libdesktop_vertex_shaders)
            set(profile vs_4_0)
        else()
            set(profile ps_4_0)
        endif()

        set(out "${libdesktop_render_shader_dir}/${shader}.cso")
        add_custom_command(
            OUTPUT "${out}"
            COMMAND "${FXC_EXECUTABLE}" /nologo /T ${profile} /E main
                /Fo "${out}" "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${shader}.hlsl"
            MAIN_DEPENDENCY "${CMAKE_CURRENT_SOURCE_DIR}/src/render/shaders/${shader}.hlsl"
            DEPENDS ${libdesktop_shader_includes}
            COMMENT "Compiling ${shader}.hlsl (${profile})"
            VERBATIM)
        list(APPEND libdesktop_render_shader_outputs "${out}")
        list(APPEND libdesktop_render_shader_symbols "libdesktop_shader_${shader}")
    endforeach()

    set(libdesktop_render_shader_cpp "${CMAKE_CURRENT_BINARY_DIR}/libdesktop_render_shaders.cpp")
    add_custom_command(
        OUTPUT "${libdesktop_render_shader_cpp}"
        COMMAND "${CMAKE_COMMAND}"
            "-DOUTPUT=${libdesktop_render_shader_cpp}"
            "-DINPUTS=${libdesktop_render_shader_outputs}"
            "-DSYMBOLS=${libdesktop_render_shader_symbols}"
            -P "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary_resources.cmake"
        DEPENDS ${libdesktop_render_shader_outputs}
            "${CMAKE_CURRENT_SOURCE_DIR}/cmake/embed_binary_resources.cmake"
        COMMENT "Embedding libdesktop-render shaders"
        VERBATIM)
    set_source_files_properties("${libdesktop_render_shader_cpp}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_sources(libdesktop-render PRIVATE "${libdesktop_render_shader_cpp}")
    add_custom_target(libdesktop_render_shaders DEPENDS ${libdesktop_render_shader_outputs})
    add_dependencies(libdesktop-render libdesktop_render_shaders)

    target_link_libraries(libdesktop-render PUBLIC d3d11 dxgi dwrite dxguid windowscodecs)
else()
    include("${CMAKE_CURRENT_SOURCE_DIR}/cmake/sdl3.cmake")
    target_sources(libdesktop-render PRIVATE src/render/linux/text.cpp src/render/linux/window_device.cpp)

    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBDESKTOP_TEXT_STACK REQUIRED IMPORTED_TARGET GLOBAL pangoft2 fontconfig freetype2)
    target_link_libraries(libdesktop-render PRIVATE PkgConfig::LIBDESKTOP_TEXT_STACK SDL3::SDL3-static)
endif()
