# libdesktop-ui: reusable controls, elements, and the portable frame layer.

set(libdesktop_ui_headers
    include/libdesktop/ui.h
    include/libdesktop/ui/app.h
    include/libdesktop/ui/command.h
    include/libdesktop/ui/controls.h
    include/libdesktop/ui/elements.h
    include/libdesktop/ui/frame.h
    include/libdesktop/ui/fwd.h
    include/libdesktop/ui/style.h
    include/libdesktop/ui/syntax.h
    include/libdesktop/ui/text_types.h
    include/libdesktop/ui/menu_bar.h
    include/libdesktop/ui/test_support.h
    include/libdesktop/ui/views.h
    include/libdesktop/ui/text/text_line.h
    include/libdesktop/ui/text/text_buffer.h
    include/libdesktop/ui/text/view_host.h
    include/libdesktop/ui/text/spell.h
    include/libdesktop/ui/text/markdown.h
    include/libdesktop/ui/text/table_layout.h
    include/libdesktop/ui/text/clipboard.h
    include/libdesktop/ui/views/view_theme.h
    include/libdesktop/ui/views/scrollbar.h
    include/libdesktop/ui/views/split_element.h
    include/libdesktop/ui/views/view_base.h
    include/libdesktop/ui/views/text_view.h
    include/libdesktop/ui/views/doc_view.h
    include/libdesktop/ui/views/read_only_doc_view.h
    include/libdesktop/ui/views/edit_doc_view.h
    include/libdesktop/ui/views/list_view.h
    include/libdesktop/ui/views/hex_view.h
    include/libdesktop/ui/views/csv_view.h
    include/libdesktop/ui/views/markdown_view.h
    src/ui/controls_internal.h
)

add_library(libdesktop-ui STATIC
    ${libdesktop_ui_headers}
    src/ui/elements.cpp
    src/ui/flex.cpp
    src/ui/controls.cpp
    src/ui/edit.cpp
    src/ui/menu.cpp
    src/ui/bubble.cpp
    src/ui/frame.cpp
    src/ui/ui.cpp
    src/ui/syntax.cpp
    src/ui/menu_bar.cpp
    src/ui/text/text_buffer.cpp
    src/ui/text/spell.cpp
    src/ui/text/table_layout.cpp
    src/ui/text/markdown.cpp
    src/ui/text/clipboard.cpp
    src/ui/views/view_theme.cpp
    src/ui/views/scrollbar.cpp
    src/ui/views/split_element.cpp
    src/ui/views/text_view.cpp
    src/ui/views/doc_view.cpp
    src/ui/views/list_view.cpp
    src/ui/views/hex_view.cpp
    src/ui/views/csv_view.cpp
    src/ui/views/markdown_view.cpp
)
add_library(libdesktop::ui ALIAS libdesktop-ui)

target_compile_features(libdesktop-ui PUBLIC cxx_std_20)
target_link_libraries(libdesktop-ui PUBLIC libdesktop::render)
target_include_directories(libdesktop-ui PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/include")
target_precompile_headers(libdesktop-ui PRIVATE "${CMAKE_CURRENT_SOURCE_DIR}/include/libdesktop/ui.h")

if(MSVC)
    target_compile_options(libdesktop-ui PRIVATE /bigobj /W3 /sdl)
    libdesktop_enable_msvc_exceptions(libdesktop-ui)
elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    target_compile_options(libdesktop-ui PRIVATE -Wall -Wno-unknown-pragmas -Wno-multichar)
endif()
