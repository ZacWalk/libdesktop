# libdesktop-render: the desktop UI that began as Diffractor's - the ui:: types, drawing interfaces,
# frames and controls, the Direct3D 11 and software renderers, DirectWrite text, WIC, Media
# Foundation movie writing and WASAPI sound - with stand-ins on Linux. CMakeLists.txt includes this
# when LIBDESKTOP_RENDER is on; docs/render.md describes the module.

add_library(libdesktop-render STATIC
    include/libdesktop/render.h
    include/libdesktop/platform_render.h
    include/libdesktop/render_software.h
    include/libdesktop/ui.h
    src/render/render_color.cpp
    src/render/render_image.cpp
    src/render/render_software.cpp
    src/render/render_surface.cpp
    src/render/ui.cpp
)
add_library(libdesktop::render ALIAS libdesktop-render)

target_compile_features(libdesktop-render PUBLIC cxx_std_20)
target_link_libraries(libdesktop-render PUBLIC libdesktop::system)
target_precompile_headers(libdesktop-render PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/render.h")

if(MSVC)
    target_compile_options(libdesktop-render PRIVATE /bigobj /W3 /sdl)
else()
    target_compile_options(libdesktop-render PRIVATE -Wall -Wno-unknown-pragmas -Wno-multichar)
endif()

if(WIN32)
    target_sources(libdesktop-render PRIVATE
        include/libdesktop/platform_win_render.h
        include/libdesktop/platform_win_render_res.h
        include/libdesktop/platform_win_ui_internal.h
        include/libdesktop/platform_win_visual.h
        src/render/platform_win_controls.cpp
        src/render/platform_win_d3d11.cpp
        src/render/platform_win_encode.cpp
        src/render/platform_win_font.cpp
        src/render/platform_win_software.cpp
        src/render/platform_win_sound.cpp
        src/render/platform_win_ui.cpp
        src/render/platform_win_wic.cpp
    )

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
        wtsapi32 ws2_32 mfplat mfreadwrite mfuuid dwmapi uxtheme comctl32 winmm imm32 version)
else()
    target_sources(libdesktop-render PRIVATE src/render/platform_linux_ui.cpp)
endif()

# The resource compiler tracks an application's script alone, not what it includes, so this names
# the module's resources and shaders as the script's dependencies: a changed cursor or shader then
# rebuilds it rather than leaving the binary with the old copy. Call it with the script that includes
# platform_win_render.rc.
function(libdesktop_render_resources rc_file)
    get_property(depends GLOBAL PROPERTY LIBDESKTOP_RENDER_RC_DEPENDS)
    set_property(SOURCE "${rc_file}" APPEND PROPERTY OBJECT_DEPENDS ${depends})
endfunction()
