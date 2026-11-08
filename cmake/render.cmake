# libdesktop-render: the desktop UI that began as Diffractor's - the ui:: types, drawing interfaces,
# frames, the element tree and controls, the Direct3D 11 and software renderers, DirectWrite text, WIC, Media
# Foundation movie writing and WASAPI sound - with stand-ins on Linux. CMakeLists.txt includes this
# when LIBDESKTOP_RENDER is on; docs/render.md describes the module.

add_library(libdesktop-render STATIC
    include/libdesktop/render.h
    include/libdesktop/platform_render.h
    include/libdesktop/render_controls.h
    include/libdesktop/render_software.h
    include/libdesktop/ui.h
    include/libdesktop/ui_elements.h
    src/render/render_bubble.cpp
    src/render/render_color.cpp
    src/render/render_controls.cpp
    src/render/render_controls_internal.h
    src/render/render_edit.cpp
    src/render/render_image.cpp
    src/render/render_menu.cpp
    src/render/render_software.cpp
    src/render/render_software_context.cpp
    src/render/render_surface.cpp
    src/render/ui.cpp
    src/render/ui_elements.cpp
    src/render/ui_flex.cpp
)
add_library(libdesktop::render ALIAS libdesktop-render)

target_compile_features(libdesktop-render PUBLIC cxx_std_20)
target_link_libraries(libdesktop-render PUBLIC libdesktop::system)
target_precompile_headers(libdesktop-render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/render.h")

if(MSVC)
    target_compile_options(libdesktop-render PRIVATE /bigobj /W3 /sdl)
    libdesktop_enable_msvc_exceptions(libdesktop-render)
else()
    target_compile_options(libdesktop-render PRIVATE -Wall -Wno-unknown-pragmas -Wno-multichar)
endif()

if(WIN32)
    target_compile_definitions(libdesktop-render PRIVATE UNICODE _UNICODE)
    target_sources(libdesktop-render PRIVATE
        include/libdesktop/platform_win_render.h
        include/libdesktop/platform_win_render_res.h
        include/libdesktop/platform_win_ui_internal.h
        include/libdesktop/platform_win_visual.h
        src/render/platform_win_d3d11.cpp
        src/render/platform_win_encode.cpp
        src/render/platform_win_font.cpp
        src/render/platform_win_software.cpp
        src/render/platform_win_sound.cpp
        src/render/platform_win_ui.cpp
        src/render/platform_win_wic.cpp
    )

    # The audio endpoint is libdesktop-os's (include/libdesktop/os.h), wrapped in platform::audio_output.
    target_link_libraries(libdesktop-render PRIVATE libdesktop::os)

    # The SDK installs one fxc per version, and none of them is on the path.
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

    # Shader model 4.0 throughout; vs.hlsl is the only vertex shader.
    set(libdesktop_pixel_shaders circle_ps font_ps pano_ps rgb_bicubic_ps rgb_ps solid_ps yuv_bicubic_ps yuv_ps
        yuv_hdr_bicubic_ps yuv_hdr_ps)
    set(libdesktop_vertex_shaders vs)
    set(libdesktop_render_shader_outputs "")

    # fxc reports no includes to the generator, so each shader depends on every shared header:
    # otherwise editing one leaves the shaders built from it stale, and the binary embeds the old code.
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
    endforeach()

    add_custom_target(libdesktop_render_shaders DEPENDS ${libdesktop_render_shader_outputs})
    add_dependencies(libdesktop-render libdesktop_render_shaders)

    # A static library carries no resources, so the module's travel as a script an application's
    # own includes (src/render/res/platform_win_render.rc). It names its files without a path:
    # these directories, on the application's include path, are what find them.
    target_include_directories(libdesktop-render INTERFACE
        "${CMAKE_CURRENT_SOURCE_DIR}/src/render/res" "${libdesktop_render_shader_dir}")

    file(GLOB libdesktop_render_resource_files CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/render/res/*")
    set_property(GLOBAL PROPERTY LIBDESKTOP_RENDER_RC_DEPENDS
        ${libdesktop_render_resource_files}
        "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/platform_win_render_res.h"
        ${libdesktop_render_shader_outputs})

    # The sources also name their libraries with #pragma comment(lib); these are what a link needs
    # besides, measured by linking the module's objects alone.
    target_link_libraries(libdesktop-render PUBLIC d3d11 dxgi dwrite dxguid windowscodecs shlwapi shcore
        wtsapi32 ws2_32 mfplat mfreadwrite mfuuid dwmapi winmm imm32 version)
else()
    target_sources(libdesktop-render PRIVATE
        src/render/platform_linux_text.cpp
        src/render/platform_linux_ui.cpp
        src/render/platform_linux_window.cpp
    )

    # Text is Pango's, rasterised with FreeType; fonts are found through fontconfig. GLOBAL so a
    # consumer linking this static library can resolve the imported target from its own directory.
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(LIBDESKTOP_TEXT_STACK REQUIRED IMPORTED_TARGET GLOBAL pangoft2 fontconfig freetype2)
    target_link_libraries(libdesktop-render PRIVATE PkgConfig::LIBDESKTOP_TEXT_STACK)

    # Windows, input and the rest of the desktop are SDL's - Wayland and X11 alike - at the pin in
    # cmake/dd-dependencies.json, built static. SDL loads each system library it drives at run time, so
    # a machine without X11, say, still runs on Wayland. What the window layer does not use is left out.
    dd_dependency_arguments("${CMAKE_CURRENT_SOURCE_DIR}/cmake/dd-dependencies.json" sdl3 libdesktop_sdl3_acquisition)
    foreach(option IN ITEMS SDL_SHARED SDL_TEST_LIBRARY SDL_TESTS SDL_EXAMPLES SDL_INSTALL SDL_CAMERA SDL_HAPTIC
                            SDL_HIDAPI SDL_JOYSTICK SDL_SENSOR SDL_POWER SDL_GPU)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(sdl3 ${libdesktop_sdl3_acquisition})
    FetchContent_MakeAvailable(sdl3)
    target_link_libraries(libdesktop-render PRIVATE SDL3::SDL3-static)

    # Windows carries the module's art as resources; elsewhere it is compiled in. Each file becomes a
    # byte array in one generated translation unit, and is a configure dependency so an edited file is
    # read again rather than leaving the build with the old copy.
    set(libdesktop_render_embedded "${CMAKE_CURRENT_BINARY_DIR}/libdesktop_render_embedded.cpp")
    set(libdesktop_render_embedded_text "// Generated from src/render/res by CMake. Do not edit.\n#include <cstddef>\n")
    foreach(pair IN ITEMS "libdesktop_shadow_png=shadow.png")
        string(REPLACE "=" ";" pair "${pair}")
        list(GET pair 0 symbol)
        list(GET pair 1 file)
        set(path "${CMAKE_CURRENT_SOURCE_DIR}/src/render/res/${file}")
        set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${path}")
        file(READ "${path}" hex HEX)
        string(REGEX REPLACE "([0-9a-f][0-9a-f])" "0x\\1," bytes "${hex}")
        string(LENGTH "${hex}" hex_length)
        math(EXPR length "${hex_length} / 2")
        string(APPEND libdesktop_render_embedded_text
            "extern const unsigned char ${symbol}[] = {${bytes}};\nextern const size_t ${symbol}_size = ${length};\n")
    endforeach()
    file(WRITE "${libdesktop_render_embedded}.in" "${libdesktop_render_embedded_text}")
    configure_file("${libdesktop_render_embedded}.in" "${libdesktop_render_embedded}" COPYONLY)
    set_source_files_properties("${libdesktop_render_embedded}" PROPERTIES SKIP_PRECOMPILE_HEADERS ON)
    target_sources(libdesktop-render PRIVATE "${libdesktop_render_embedded}")
endif()

# An application's entry point, which it links itself: target_link_libraries(app PRIVATE
# libdesktop::render_main). On Linux it is an object, never a member of the archive, because GNU ld
# takes main from the first archive on the line that defines one, and an application's other archives
# can carry a tool's own (bzip2's, Highway's), which then runs instead of the application. wWinMain has
# no such rival and stays in the archive, so on Windows this target is empty.
if(WIN32)
    add_library(libdesktop-render-main INTERFACE)
else()
    add_library(libdesktop-render-main OBJECT src/render/platform_linux_main.cpp)
    target_link_libraries(libdesktop-render-main PRIVATE libdesktop::render)
endif()
add_library(libdesktop::render_main ALIAS libdesktop-render-main)

# The resource compiler tracks an application's script alone, not what it includes, so this names
# the module's resources and shaders as the script's dependencies: a changed cursor or shader then
# rebuilds it rather than leaving the binary with the old copy. Call it with the script that includes
# platform_win_render.rc.
function(libdesktop_render_resources rc_file)
    get_property(depends GLOBAL PROPERTY LIBDESKTOP_RENDER_RC_DEPENDS)
    set_property(SOURCE "${rc_file}" APPEND PROPERTY OBJECT_DEPENDS ${depends})
endfunction()
