# FindCEF.cmake - Find CEF binary distribution

if(NOT CEF_ROOT)
    message(FATAL_ERROR "CEF_ROOT not set. Download CEF from https://cef-builds.spotifycdn.com/index.html and extract to third_party/cef/")
endif()

if(NOT EXISTS "${CEF_ROOT}/include/cef_version.h")
    message(FATAL_ERROR "CEF not found at ${CEF_ROOT}. Ensure CEF binary distribution is extracted there.")
endif()

# Read CEF version
file(READ "${CEF_ROOT}/include/cef_version.h" CEF_VERSION_CONTENT)
string(REGEX MATCH "CEF_VERSION \"([^\"]+)\"" _ ${CEF_VERSION_CONTENT})
set(CEF_VERSION ${CMAKE_MATCH_1})
message(STATUS "Found CEF: ${CEF_VERSION}")

set(CEF_INCLUDE_DIRS "${CEF_ROOT}" "${CEF_ROOT}/include")

# Platform-specific library setup
if(WIN32)
    set(CEF_LIB_DIR "${CEF_ROOT}/Release")
    set(CEF_LIBRARIES
        "${CEF_LIB_DIR}/libcef.lib"
        "${CEF_ROOT}/libcef_dll_wrapper/Release/libcef_dll_wrapper.lib"
    )
elseif(APPLE)
    set(CEF_LIBRARIES
        "${CEF_ROOT}/Release/Chromium Embedded Framework.framework"
        "${CEF_ROOT}/build/libcef_dll_wrapper/Release/libcef_dll_wrapper.a"
    )
else() # Linux
    set(CEF_LIB_DIR "${CEF_ROOT}/Release")
    set(CEF_LIBRARIES
        "${CEF_LIB_DIR}/libcef.so"
        "${CEF_ROOT}/build/libcef_dll_wrapper/libcef_dll_wrapper.a"
    )
endif()

# Build wrapper library if not present
if(APPLE)
    set(CEF_WRAPPER_PATH "${CEF_ROOT}/build/libcef_dll_wrapper/Release/libcef_dll_wrapper.a")
else()
    set(CEF_WRAPPER_PATH "${CEF_ROOT}/build/libcef_dll_wrapper/libcef_dll_wrapper.a")
endif()

if(NOT EXISTS "${CEF_WRAPPER_PATH}")
    # Check if wrapper exists without Release/ subdirectory (CEF build output location)
    set(CEF_WRAPPER_ACTUAL "${CEF_ROOT}/build/libcef_dll_wrapper/libcef_dll_wrapper.a")
    if(EXISTS "${CEF_WRAPPER_ACTUAL}" AND APPLE)
        # Create Release/ symlink for macOS (CEF builds to parent dir, we expect Release/)
        message(STATUS "Creating symlink for libcef_dll_wrapper...")
        file(MAKE_DIRECTORY "${CEF_ROOT}/build/libcef_dll_wrapper/Release")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -E create_symlink
                "${CEF_WRAPPER_ACTUAL}"
                "${CEF_WRAPPER_PATH}"
        )
    else()
        message(STATUS "Building libcef_dll_wrapper...")
        execute_process(
            COMMAND ${CMAKE_COMMAND} -B build -DCMAKE_BUILD_TYPE=Release
            WORKING_DIRECTORY ${CEF_ROOT}
            RESULT_VARIABLE CEF_CONFIG_RESULT
        )
        if(NOT CEF_CONFIG_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to configure CEF")
        endif()
        # Limit parallelism - CEF wrapper builds OOM with unlimited -j even on 16GB RAM
        execute_process(
            COMMAND ${CMAKE_COMMAND} --build build --target libcef_dll_wrapper -j2
            WORKING_DIRECTORY ${CEF_ROOT}
            RESULT_VARIABLE CEF_BUILD_RESULT
        )
        if(NOT CEF_BUILD_RESULT EQUAL 0)
            message(FATAL_ERROR "Failed to build libcef_dll_wrapper")
        endif()
        # Create Release/ symlink on macOS after build
        if(APPLE AND EXISTS "${CEF_WRAPPER_ACTUAL}" AND NOT EXISTS "${CEF_WRAPPER_PATH}")
            file(MAKE_DIRECTORY "${CEF_ROOT}/build/libcef_dll_wrapper/Release")
            execute_process(
                COMMAND ${CMAKE_COMMAND} -E create_symlink
                    "${CEF_WRAPPER_ACTUAL}"
                    "${CEF_WRAPPER_PATH}"
            )
        endif()
    endif()
endif()

set(CEF_FOUND TRUE)
