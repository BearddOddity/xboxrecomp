# gui: immediate-mode GUI in C (after Dear ImGui) and the runtime overlay
# (performance panel, debug menu with title-registered pages, notices).
#
#   gui.c          portable core: widgets, layout, navigation, draw lists
#   gui_d3d11.c    Direct3D 11 renderer for the core's draw data
#   gui_overlay.c  the overlay any title can turn on (Windows)

if(WIN32)
    add_library(xbox_gui STATIC
        gui.c
        gui_d3d11.c
        gui_overlay.c
    )
    target_link_libraries(xbox_gui PUBLIC xbox_input d3d11 dxgi dxguid d3dcompiler pdh psapi xinput)
else()
    add_library(xbox_gui STATIC
        gui.c
    )
    target_link_libraries(xbox_gui PUBLIC m)
endif()

target_include_directories(xbox_gui PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})

if(MSVC)
    target_compile_options(xbox_gui PRIVATE /W4 /O2)
else()
    target_compile_options(xbox_gui PRIVATE -Wall -Wextra -O2)
endif()

