cmake_minimum_required(VERSION 3.19)
project(jellyfin-desktop-cef LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# SDL2
find_package(SDL2 REQUIRED)

# OpenGL
find_package(OpenGL REQUIRED)

add_executable(jellyfin-desktop
    src/main.cpp
)

target_include_directories(jellyfin-desktop PRIVATE
    ${SDL2_INCLUDE_DIRS}
)

target_link_libraries(jellyfin-desktop PRIVATE
    ${SDL2_LIBRARIES}
    OpenGL::GL
)
