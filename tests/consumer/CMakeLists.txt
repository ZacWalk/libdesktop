cmake_minimum_required(VERSION 3.21)
project(platform_consumer LANGUAGES CXX)

include(FetchContent)
FetchContent_Declare(platform_h SOURCE_DIR "${PLATFORM_SOURCE_DIR}")
FetchContent_MakeAvailable(platform_h)
if(TARGET platform_tests OR TARGET platform_http_tests)
    message(FATAL_ERROR "A child consumer must not inherit platform test targets")
endif()

platform_add_app(consumer CONSOLE SOURCES main.cpp)
