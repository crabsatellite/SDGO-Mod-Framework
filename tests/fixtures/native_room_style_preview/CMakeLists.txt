cmake_minimum_required(VERSION 3.21)
project(room_style_preview_prototype LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This prototype targets the Win32 SDGO client")
endif()
if(CMAKE_SIZEOF_VOID_P EQUAL 8)
    message(FATAL_ERROR "Configure with -A Win32")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_MSVC_RUNTIME_LIBRARY "MultiThreaded$<$<CONFIG:Debug>:Debug>")

set(
    SDGO_FRAMEWORK_INCLUDE_DIR
    "${CMAKE_CURRENT_LIST_DIR}/../../../include"
    CACHE PATH
    "Directory containing sdgo/sdgo_api.h")
if(NOT EXISTS "${SDGO_FRAMEWORK_INCLUDE_DIR}/sdgo/sdgo_api.h")
    message(
        FATAL_ERROR
        "Set SDGO_FRAMEWORK_INCLUDE_DIR to the framework include directory")
endif()

option(
    ROOM_STYLE_PREVIEW_WITH_OVERLAY
    "Install the native pack overlay used by the 12-slot normal-room preview"
    ON)
option(
    ROOM_STYLE_PREVIEW_MISSION_OVERLAY
    "Bind the native pack overlay to the cooperative mission room resource"
    OFF)

add_library(
    room_style_preview SHARED
    room_style_preview.cpp)
if(ROOM_STYLE_PREVIEW_WITH_OVERLAY)
    target_sources(room_style_preview PRIVATE room_style_preview_overlay.cpp)
    target_compile_definitions(
        room_style_preview PRIVATE ROOM_STYLE_PREVIEW_WITH_OVERLAY=1)
    if(ROOM_STYLE_PREVIEW_MISSION_OVERLAY)
        target_compile_definitions(
            room_style_preview PRIVATE ROOM_STYLE_PREVIEW_MISSION_OVERLAY=1)
    endif()
    target_link_libraries(room_style_preview PRIVATE bcrypt)
endif()
target_include_directories(
    room_style_preview PRIVATE
    "${SDGO_FRAMEWORK_INCLUDE_DIR}")
target_compile_definitions(
    room_style_preview PRIVATE
    WIN32_LEAN_AND_MEAN NOMINMAX _CRT_SECURE_NO_WARNINGS)
target_compile_options(
    room_style_preview PRIVATE
    /W4 /permissive- /utf-8)
set_target_properties(
    room_style_preview PROPERTIES
    PREFIX ""
    OUTPUT_NAME "room_style_preview")

add_executable(room_style_preview_offline_test offline_test.cpp)
target_compile_options(
    room_style_preview_offline_test PRIVATE
    /W4 /permissive- /utf-8)

enable_testing()
add_test(
    NAME room_style_preview_offline_test
    COMMAND room_style_preview_offline_test)
