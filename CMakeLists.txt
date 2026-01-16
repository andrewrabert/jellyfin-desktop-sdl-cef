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
    find_library(MEDIAPLAYER_FRAMEWORK MediaPlayer REQUIRED)

    set(PLATFORM_SOURCES
        src/platform/macos_layer.mm
        src/platform/macos_app.mm
        src/compositor/metal_compositor.mm
        src/player/media_session.cpp
        src/player/macos/media_session_macos.mm
    )
    set(PLATFORM_LIBRARIES
        ${COCOA_FRAMEWORK}
        ${METAL_FRAMEWORK}
        ${QUARTZCORE_FRAMEWORK}
        ${IOSURFACE_FRAMEWORK}
        ${MEDIAPLAYER_FRAMEWORK}
    )
else()
    # Linux: OpenGL/EGL, Wayland
    find_package(OpenGL REQUIRED COMPONENTS EGL OpenGL)
    find_package(PkgConfig REQUIRED)
    pkg_check_modules(WAYLAND_EGL REQUIRED wayland-egl)
    pkg_check_modules(LIBDRM REQUIRED libdrm)
    pkg_check_modules(WAYLAND REQUIRED wayland-client)
    pkg_check_modules(WAYLAND_PROTOCOLS REQUIRED wayland-protocols)
    pkg_check_modules(SYSTEMD REQUIRED libsystemd)

    # Find wayland-scanner
    find_program(WAYLAND_SCANNER wayland-scanner REQUIRED)
    pkg_get_variable(WAYLAND_PROTOCOLS_DIR wayland-protocols pkgdatadir)

    # Generate Wayland protocol sources
    set(WAYLAND_GENERATED_DIR "${CMAKE_BINARY_DIR}/wayland-protocols")
    file(MAKE_DIRECTORY ${WAYLAND_GENERATED_DIR})

    set(COLOR_MGMT_PROTOCOL "${WAYLAND_PROTOCOLS_DIR}/staging/color-management/color-management-v1.xml")
    set(COLOR_MGMT_CLIENT_H "${WAYLAND_GENERATED_DIR}/color-management-v1-client.h")
    set(COLOR_MGMT_CODE_C "${WAYLAND_GENERATED_DIR}/color-management-v1-code.c")

    add_custom_command(
        OUTPUT ${COLOR_MGMT_CLIENT_H}
        COMMAND ${WAYLAND_SCANNER} client-header ${COLOR_MGMT_PROTOCOL} ${COLOR_MGMT_CLIENT_H}
        DEPENDS ${COLOR_MGMT_PROTOCOL}
        COMMENT "Generating color-management-v1 client header"
    )
    add_custom_command(
        OUTPUT ${COLOR_MGMT_CODE_C}
        COMMAND ${WAYLAND_SCANNER} private-code ${COLOR_MGMT_PROTOCOL} ${COLOR_MGMT_CODE_C}
        DEPENDS ${COLOR_MGMT_PROTOCOL}
        COMMENT "Generating color-management-v1 code"
    )

    set(PLATFORM_SOURCES
        src/context/egl_context.cpp
        src/platform/wayland_subsurface.cpp
        src/compositor/opengl_compositor.cpp
        src/player/media_session.cpp
        src/player/mpris/media_session_mpris.cpp
        ${COLOR_MGMT_CODE_C}
        ${COLOR_MGMT_CLIENT_H}
    )
    set(PLATFORM_LIBRARIES
        ${WAYLAND_LIBRARIES}
        ${WAYLAND_EGL_LIBRARIES}
        ${SYSTEMD_LIBRARIES}
        OpenGL::EGL
        OpenGL::OpenGL
    )
    set(PLATFORM_INCLUDE_DIRS
        ${WAYLAND_INCLUDE_DIRS}
        ${WAYLAND_EGL_INCLUDE_DIRS}
        ${LIBDRM_INCLUDE_DIRS}
        ${SYSTEMD_INCLUDE_DIRS}
        ${CMAKE_BINARY_DIR}
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
    BYPRODUCTS ${MPV_LIBRARY}
)

set(MPV_INCLUDE_DIRS "${MPV_SOURCE_DIR}/include")
set(MPV_LIBRARIES "${MPV_LIBRARY}")

# Embed JS shims into generated header
set(JS_SHIMS
    ${CMAKE_SOURCE_DIR}/src/web/native-shim.js
    ${CMAKE_SOURCE_DIR}/src/web/mpv-player-core.js
    ${CMAKE_SOURCE_DIR}/src/web/mpv-video-player.js
    ${CMAKE_SOURCE_DIR}/src/web/mpv-audio-player.js
    ${CMAKE_SOURCE_DIR}/src/web/input-plugin.js
)
set(EMBEDDED_JS_HEADER ${CMAKE_BINARY_DIR}/generated/embedded_js.h)
file(MAKE_DIRECTORY ${CMAKE_BINARY_DIR}/generated)

add_custom_command(
    OUTPUT ${EMBEDDED_JS_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DJS_FILES="${JS_SHIMS}"
        -DOUTPUT_FILE=${EMBEDDED_JS_HEADER}
        -P ${CMAKE_SOURCE_DIR}/cmake/embed_resources.cmake
    DEPENDS ${JS_SHIMS}
    COMMENT "Embedding JS shims"
)
add_custom_target(embedded_js DEPENDS ${EMBEDDED_JS_HEADER})

# Embed ALL web resources into generated header
set(EMBEDDED_RESOURCES_HEADER ${CMAKE_BINARY_DIR}/generated/embedded_resources.h)
file(GLOB_RECURSE WEB_RESOURCE_FILES CONFIGURE_DEPENDS "${CMAKE_SOURCE_DIR}/src/web/*")

add_custom_command(
    OUTPUT ${EMBEDDED_RESOURCES_HEADER}
    COMMAND ${CMAKE_COMMAND}
        -DRESOURCE_DIR="${CMAKE_SOURCE_DIR}/src/web"
        -DOUTPUT_FILE=${EMBEDDED_RESOURCES_HEADER}
        -DBASE_URL="resources"
        -P ${CMAKE_SOURCE_DIR}/cmake/embed_all_resources.cmake
    DEPENDS ${WEB_RESOURCE_FILES}
    COMMENT "Embedding web resources"
)
add_custom_target(embedded_resources DEPENDS ${EMBEDDED_RESOURCES_HEADER})

add_executable(jellyfin-desktop
    src/main.cpp
    src/cef/cef_app.cpp
    src/cef/cef_client.cpp
    src/cef/resource_handler.cpp
    src/context/vulkan_context.cpp
    src/player/mpv/mpv_player_vk.cpp
    src/settings.cpp
    src/ui/menu_overlay.cpp
    ${PLATFORM_SOURCES}
)

# Ensure JS shims are embedded before compiling cef_app.cpp
add_dependencies(jellyfin-desktop embedded_js embedded_resources)

target_include_directories(jellyfin-desktop PRIVATE
    ${CEF_INCLUDE_DIRS}
    ${MPV_INCLUDE_DIRS}
    ${Vulkan_INCLUDE_DIRS}
    ${PLATFORM_INCLUDE_DIRS}
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_BINARY_DIR}/generated
)

target_link_libraries(jellyfin-desktop PRIVATE
    ${CEF_LIBRARIES}
    SDL3::SDL3
    ${MPV_LIBRARIES}
    Vulkan::Vulkan
    ${PLATFORM_LIBRARIES}
)

# Enable ARC for Objective-C++ files on macOS
if(APPLE)
    set_source_files_properties(
        src/platform/macos_layer.mm
        src/platform/macos_app.mm
        src/compositor/metal_compositor.mm
        src/player/macos/media_session_macos.mm
        PROPERTIES COMPILE_FLAGS "-fobjc-arc"
    )
endif()

# Linux RPATH configuration for libcef.so loading
if(UNIX AND NOT APPLE)
    set_target_properties(jellyfin-desktop PROPERTIES
        INSTALL_RPATH "$ORIGIN"
        BUILD_WITH_INSTALL_RPATH TRUE
    )
endif()

# Copy CEF resources to build directory
if(APPLE)
    # macOS: Copy framework to build/Frameworks/ and fix install_name paths
    # CEF ships with @executable_path/../Frameworks/... but we want @executable_path/Frameworks/...
    set(CEF_FRAMEWORK_NAME "Chromium Embedded Framework")
    set(CEF_FRAMEWORK_DIR "${CMAKE_BINARY_DIR}/Frameworks/${CEF_FRAMEWORK_NAME}.framework")
    set(CEF_FRAMEWORK_LIB "${CEF_FRAMEWORK_DIR}/${CEF_FRAMEWORK_NAME}")
    set(OLD_INSTALL_NAME "@executable_path/../Frameworks/${CEF_FRAMEWORK_NAME}.framework/${CEF_FRAMEWORK_NAME}")
    set(NEW_INSTALL_NAME "@executable_path/Frameworks/${CEF_FRAMEWORK_NAME}.framework/${CEF_FRAMEWORK_NAME}")

    add_custom_command(TARGET jellyfin-desktop POST_BUILD
        # Copy framework
        COMMAND ${CMAKE_COMMAND} -E make_directory "${CMAKE_BINARY_DIR}/Frameworks"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_ROOT}/Release/${CEF_FRAMEWORK_NAME}.framework"
            "${CEF_FRAMEWORK_DIR}"
        # Fix framework's install_name
        COMMAND install_name_tool -id "${NEW_INSTALL_NAME}" "${CEF_FRAMEWORK_LIB}"
        # Fix executable's reference to framework
        COMMAND install_name_tool -change "${OLD_INSTALL_NAME}" "${NEW_INSTALL_NAME}" "$<TARGET_FILE:jellyfin-desktop>"
        COMMENT "Copying CEF framework and fixing install_name paths"
    )
else()
    # Linux: Separate Resources and Release directories
    add_custom_command(TARGET jellyfin-desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_ROOT}/Resources"
            "$<TARGET_FILE_DIR:jellyfin-desktop>"
        COMMAND ${CMAKE_COMMAND} -E copy_directory
            "${CEF_ROOT}/Release"
            "$<TARGET_FILE_DIR:jellyfin-desktop>"
    )
endif()

add_dependencies(jellyfin-desktop mpv_build)

# Copy libmpv to build directory and fix install_name
if(APPLE)
    add_custom_command(TARGET jellyfin-desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${MPV_BUILD_DIR}/libmpv.2.dylib"
            "$<TARGET_FILE_DIR:jellyfin-desktop>/libmpv.2.dylib"
        # Fix libmpv install_name to load from executable directory
        COMMAND install_name_tool -id "@executable_path/libmpv.2.dylib"
            "$<TARGET_FILE_DIR:jellyfin-desktop>/libmpv.2.dylib"
        # Fix executable to use local libmpv
        COMMAND install_name_tool -change "@rpath/libmpv.2.dylib"
            "@executable_path/libmpv.2.dylib" "$<TARGET_FILE:jellyfin-desktop>"
    )
else()
    add_custom_command(TARGET jellyfin-desktop POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy
            "${MPV_BUILD_DIR}/libmpv.so.2"
            "$<TARGET_FILE_DIR:jellyfin-desktop>/libmpv.so.2"
    )
endif()
