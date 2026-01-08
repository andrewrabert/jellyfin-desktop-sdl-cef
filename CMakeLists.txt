cmake_minimum_required(VERSION 3.19)
project(jellyfin-desktop-cef LANGUAGES C CXX)

# Enable Objective-C++ on macOS
if(APPLE)
    enable_language(OBJCXX)
endif()

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

# SDL3
find_package(SDL3 REQUIRED)

# Vulkan (for video subsurface)
find_package(Vulkan REQUIRED)

# Platform-specific dependencies
if(APPLE)
    # macOS: OpenGL via CGL, Metal frameworks
    find_package(OpenGL REQUIRED)
    find_library(COCOA_FRAMEWORK Cocoa REQUIRED)
    find_library(METAL_FRAMEWORK Metal REQUIRED)
    find_library(QUARTZCORE_FRAMEWORK QuartzCore REQUIRED)
    find_library(IOSURFACE_FRAMEWORK IOSurface REQUIRED)

    set(PLATFORM_SOURCES
        src/cgl_context.mm
        src/macos_layer.mm
    )
    set(PLATFORM_LIBRARIES
        ${COCOA_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${IOSURFACE_FRAMEWORK}
        OpenGL::GL
    )
else()
    # Linux: OpenGL/EGL, Wayland
    find_package(OpenGL REQUIRED COMPONENTS EGL OpenGL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_EGL REQUIRED wayland-egl)
    pkg_check_modules(LIBDRM REQUIRED libdrm)
    pkg_check_modules(WAYLAND REQUIRED wayland-client)

    set(PLATFORM_SOURCES
        src/egl_context.cpp
        src/wayland_subsurface.cpp
        src/wayland-protocols/color-management-v1-code.c
    )
    set(PLATFORM_LIBRARIES
        ${WAYLAND_LIBRARIES}
        ${WAYLAND_EGL_LIBRARIES}
        OpenGL::EGL
        OpenGL::OpenGL
    )
    set(PLATFORM_INCLUDE_DIRS
        ${WAYLAND_INCLUDE_DIRS}
        ${WAYLAND_EGL_INCLUDE_DIRS}
        ${LIBDRM_INCLUDE_DIRS}
    )
endif()

# Build mpv from submodule using meson
set(MPV_SOURCE_DIR "${CMAKE_SOURCE_DIR}/third_party/mpv")
set(MPV_BUILD_DIR "${MPV_SOURCE_DIR}/build")

if(APPLE)
    set(MPV_LIBRARY "${MPV_BUILD_DIR}/libmpv.dylib")
else()
    set(MPV_LIBRARY "${MPV_BUILD_DIR}/libmpv.so")
endif()

# Configure meson if not yet configured
if(NOT EXISTS "${MPV_BUILD_DIR}/build.ninja")
    message(STATUS "Configuring mpv with meson...")
    execute_process(
        COMMAND meson setup build --default-library=shared -Dlibmpv=true
        WORKING_DIRECTORY ${MPV_SOURCE_DIR}
        RESULT_VARIABLE MPV_SETUP_RESULT
    )
    if(NOT MPV_SETUP_RESULT EQUAL 0)
        message(FATAL_ERROR "Failed to configure mpv with meson")
    endif()
endif()

# Custom target that always invokes meson compile (meson handles incremental builds)
add_custom_target(mpv_build
    COMMAND meson compile -C build
    WORKING_DIRECTORY ${MPV_SOURCE_DIR}
    COMMENT "Building mpv (meson handles incremental builds)"
)

set(MPV_INCLUDE_DIRS "${MPV_SOURCE_DIR}/include")
set(MPV_LIBRARIES "${MPV_LIBRARY}")

add_executable(jellyfin-desktop
    src/main.cpp
    src/cef_app.cpp
    src/cef_client.cpp
    src/opengl_compositor.cpp
    src/vulkan_context.cpp
    src/mpv_player_vk.cpp
    src/settings.cpp
    src/menu_overlay.cpp
    ${PLATFORM_SOURCES}
)

target_include_directories(jellyfin-desktop PRIVATE
    ${CEF_INCLUDE_DIRS}
    ${MPV_INCLUDE_DIRS}
    ${Vulkan_INCLUDE_DIRS}
    ${PLATFORM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
)

target_link_libraries(jellyfin-desktop PRIVATE
    ${CEF_LIBRARIES}
    SDL3::SDL3
    ${MPV_LIBRARIES}
    Vulkan::Vulkan
    ${PLATFORM_LIBRARIES}
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

# Copy application resources (always sync when changed)
file(GLOB_RECURSE RESOURCE_FILES "${CMAKE_SOURCE_DIR}/resources/*")
add_custom_target(copy_resources ALL
    COMMAND ${CMAKE_COMMAND} -E copy_directory
        "${CMAKE_SOURCE_DIR}/resources"
        "${CMAKE_BINARY_DIR}/resources"
    DEPENDS ${RESOURCE_FILES}
    COMMENT "Copying resources"
)
add_dependencies(jellyfin-desktop mpv_build copy_resources)

# Copy libmpv to build directory
if(APPLE)
    add_custom_command(TARGET jellyfin-desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${MPV_BUILD_DIR}/libmpv.2.dylib"
            "$<TARGET_FILE_DIR:jellyfin-desktop>/libmpv.2.dylib"
    )
else()
    add_custom_command(TARGET jellyfin-desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${MPV_BUILD_DIR}/libmpv.so.2"
            "$<TARGET_FILE_DIR:jellyfin-desktop>/libmpv.so.2"
    )
endif()
