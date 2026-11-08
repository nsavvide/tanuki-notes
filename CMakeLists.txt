cmake_minimum_required(VERSION 3.25)
project(tanuki-notes C CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON) # clangd

include(cmake/CPM.cmake)
CPMAddPackage(
    NAME SDL3
    GITHUB_REPOSITORY libsdl-org/SDL
    GIT_TAG release-3.4.20
    OPTIONS "SDL_SHARED OFF" "SDL_STATIC ON" "SDL_TEST_LIBRARY OFF"
)
CPMAddPackage(
    NAME nanovg
    GITHUB_REPOSITORY memononen/nanovg
    GIT_TAG ce3bf745eb2d2dbc14a50bf2446783f691ac4353
    DOWNLOAD_ONLY YES
)
CPMAddPackage(
    NAME nlohmann_json
    GITHUB_REPOSITORY nlohmann/json
    VERSION 3.12.0
    OPTIONS "JSON_BuildTests OFF"
)

find_package(OpenGL REQUIRED)
find_package(PkgConfig REQUIRED)
pkg_check_modules(POPPLER REQUIRED IMPORTED_TARGET poppler-cpp)

# NanoVG: vector drawing (anti-aliased paths, text, images) on OpenGL.
add_library(nanovg STATIC ${nanovg_SOURCE_DIR}/src/nanovg.c)
target_include_directories(nanovg SYSTEM PUBLIC ${nanovg_SOURCE_DIR}/src)
target_compile_options(nanovg PRIVATE -w)

add_executable(tanuki-notes
    src/main.cpp
    src/app.cpp
    src/assets.cpp
    src/code.cpp
    src/node.cpp
    src/render.cpp
    src/rough.cpp
    src/selftest.cpp
    src/store.cpp
    src/text.cpp
    src/ui.cpp
    src/vim.cpp
)
target_link_libraries(tanuki-notes PRIVATE
    SDL3::SDL3 nanovg nlohmann_json::nlohmann_json OpenGL::GL
    PkgConfig::POPPLER)
target_compile_options(tanuki-notes PRIVATE -Wall -Wextra
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
target_link_options(tanuki-notes PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>)

# Fonts are compiled in (#embed in src/text.cpp), so the binary is all you need.
set_property(SOURCE src/text.cpp APPEND PROPERTY OBJECT_DEPENDS
    ${CMAKE_SOURCE_DIR}/assets/fonts/Virgil.ttf
    ${CMAKE_SOURCE_DIR}/assets/fonts/Inter-Regular.ttf
    ${CMAKE_SOURCE_DIR}/assets/fonts/Inter-Medium.ttf
    ${CMAKE_SOURCE_DIR}/assets/fonts/JetBrainsMono-Regular.ttf)
install(TARGETS tanuki-notes)
