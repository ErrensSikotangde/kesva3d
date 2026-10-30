cmake_minimum_required(VERSION 3.22.1)

set(CMAKE_EXPORT_COMPILE_COMMANDS ON CACHE BOOL "Enable compile_commands.json generation")

# C++23 标准配置，与 NDK r29 完全匹配
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

project("kesva3d")

add_library(${CMAKE_PROJECT_NAME} SHARED
    KesvaNativeMain.cpp)

target_include_directories(${CMAKE_PROJECT_NAME} PUBLIC
    ${CMAKE_SOURCE_DIR}/include
)

find_package(SDL2 REQUIRED CONFIG)

# 静态库链接顺序：上层库 → 底层依赖
target_link_libraries(${CMAKE_PROJECT_NAME}
    SDL2::SDL2
    ${CMAKE_SOURCE_DIR}/libs/arm64-v8a/bgfx/libbgfx.a
    ${CMAKE_SOURCE_DIR}/libs/arm64-v8a/bgfx/libbimg.a
    ${CMAKE_SOURCE_DIR}/libs/arm64-v8a/bgfx/libbimg_decode.a
    ${CMAKE_SOURCE_DIR}/libs/arm64-v8a/bgfx/libbimg_encode.a
    ${CMAKE_SOURCE_DIR}/libs/arm64-v8a/bgfx/libbx.a

    ${CMAKE_SOURCE_DIR}/libs/arm64-v8a/luajit/libluajit.a
    android
    log
    EGL
    GLESv2
    vulkan
)
