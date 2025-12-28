cmake_minimum_required(VERSION 3.19)
project(jellyfin-desktop-cef LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)

# Match CEF's compile flags for ABI compatibility
if(UNIX AND NOT APPLE)
    add_compile_options(
        -fno-strict-aliasing
        -fPIC
        -fstack-protector
        -funwind-tables
        -fvisibility=hidden
        --param=ssp-buffer-size=4
        -pipe
        -pthread
    )
endif()

# CEF configuration
set(CEF_ROOT "${CMAKE_SOURCE_DIR}/third_party/cef" CACHE PATH "CEF binary distribution root")
list(APPEND CMAKE_MODULE_PATH "${CMAKE_SOURCE_DIR}/cmake")
find_package(CEF REQUIRED)

# SDL2
find_package(SDL2 REQUIRED)

# Vulkan
find_package(Vulkan REQUIRED)

# Build mpv from submodule using meson
set(MPV_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/mpv")
set(MPV_BUILD_DIR "${MPV_SOURCE_DIR}/build")

if(NOT EXISTS "${MPV_BUILD_DIR}/libmpv.so")
    message(STATUS "Building mpv from submodule...")
    execute_process(
        COMMAND meson setup build --default-library=shared -Dlibmpv=true
        WORKING_DIRECTORY ${MPV_SOURCE_DIR}
        RESULT_VARIABLE MPV_SETUP_RESULT
    )
    if(NOT MPV_SETUP_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to configure mpv with meson")
    endif()
    execute_process(
        COMMAND meson compile -C build
        WORKING_DIRECTORY ${MPV_SOURCE_DIR}
        RESULT_VARIABLE MPV_BUILD_RESULT
    )
    if(NOT MPV_BUILD_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to build mpv")
    endif()
endif()

set(MPV_INCLUDE_DIRS "${MPV_SOURCE_DIR}/include")
set(MPV_LIBRARIES "${MPV_BUILD_DIR}/libmpv.so")

add_executable(jellyfin-desktop
    src/main.cpp
    src/cef_app.cpp
    src/cef_client.cpp
    src/vulkan_context.cpp
    src/vulkan_compositor.cpp
    src/mpv_player_vk.cpp
    src/settings.cpp
)

target_include_directories(jellyfin-desktop PRIVATE
    ${CEF_INCLUDE_DIRS}
    ${SDL2_INCLUDE_DIRS}
    ${MPV_INCLUDE_DIRS}
    ${Vulkan_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(jellyfin-desktop PRIVATE
    ${CEF_LIBRARIES}
    ${SDL2_LIBRARIES}
    ${MPV_LIBRARIES}
    Vulkan::Vulkan
)

# Linux RPATH configuration for libcef.so loading
if(UNIX AND NOT APPLE)
    set_target_properties(jellyfin-desktop PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# Copy CEF resources to build directory
add_custom_command(TARGET jellyfin-desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_ROOT}/Resources"
        "$<TARGET_FILE_DIR:jellyfin-desktop>"
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CEF_ROOT}/Release"
        "$<TARGET_FILE_DIR:jellyfin-desktop>"
)

# Copy application resources
add_custom_command(TARGET jellyfin-desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources"
        "$<TARGET_FILE_DIR:jellyfin-desktop>/resources"
)

# Copy libmpv to build directory
add_custom_command(TARGET jellyfin-desktop POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E copy
        "${MPV_BUILD_DIR}/libmpv.so.2"
        "$<TARGET_FILE_DIR:jellyfin-desktop>/libmpv.so.2"
)
