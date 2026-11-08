# SDL3 is shared by the platform and render Linux backends. Keep the acquisition here so the
# UI target does not own an operating-system window dependency.
if(NOT TARGET SDL3::SDL3-static)
    dd_dependency_arguments("${CMAKE_CURRENT_SOURCE_DIR}/cmake/dd-dependencies.json" sdl3
        libdesktop_sdl3_acquisition)
    foreach(option IN ITEMS SDL_SHARED SDL_TEST_LIBRARY SDL_TESTS SDL_EXAMPLES SDL_INSTALL SDL_CAMERA SDL_HAPTIC
                            SDL_HIDAPI SDL_JOYSTICK SDL_SENSOR SDL_POWER SDL_GPU)
        set(${option} OFF CACHE BOOL "" FORCE)
    endforeach()
    set(SDL_STATIC ON CACHE BOOL "" FORCE)
    FetchContent_Declare(sdl3 ${libdesktop_sdl3_acquisition})
    FetchContent_MakeAvailable(sdl3)
endif()
