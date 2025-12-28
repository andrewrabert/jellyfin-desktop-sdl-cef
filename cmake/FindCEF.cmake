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
        "${CEF_ROOT}/libcef_dll_wrapper/libcef_dll_wrapper.a"
    )
else() # Linux
    set(CEF_LIB_DIR "${CEF_ROOT}/Release")
    set(CEF_LIBRARIES
        "${CEF_LIB_DIR}/libcef.so"
        "${CEF_ROOT}/build/libcef_dll_wrapper/libcef_dll_wrapper.a"
    )
endif()

# Check for wrapper library - must be built first
if(NOT EXISTS "${CEF_ROOT}/build/libcef_dll_wrapper/libcef_dll_wrapper.a")
    message(WARNING "libcef_dll_wrapper not found. You may need to build it first:")
    message(WARNING "  cd ${CEF_ROOT} && cmake -B build && cmake --build build --target libcef_dll_wrapper")
endif()

set(CEF_FOUND TRUE)
