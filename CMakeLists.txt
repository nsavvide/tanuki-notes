cmake_minimum_required(VERSION 3.25)
project(tanuki-notes CXX)

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

add_executable(tanuki-notes src/main.cpp)
target_link_libraries(tanuki-notes PRIVATE SDL3::SDL3)
target_compile_options(tanuki-notes PRIVATE -Wall -Wextra
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
target_link_options(tanuki-notes PRIVATE
    $<$<CONFIG:Debug>:-fsanitize=address,undefined>)
