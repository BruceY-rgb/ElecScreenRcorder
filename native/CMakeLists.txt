cmake_minimum_required(VERSION 3.20)

project(recorder_core VERSION 1.0.0 LANGUAGES CXX)

# Set C++ standard
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Windows-specific settings
if(WIN32)
    add_definitions(-D_UNICODE)
    add_definitions(-DUNICODE)
    add_definitions(-D_CRT_SECURE_NO_WARNINGS)
    # Enable Unicode
    add_compile_options(/utf-8)
endif()

# Source files
set(SOURCES
    src/main.cpp
    src/protocol.cpp
    src/utils.cpp
    src/input_capture.cpp
    src/csv_writer.cpp
    src/recorder.cpp
    src/system_info.cpp
)

set(HEADERS
    src/protocol.h
    src/utils.h
    src/input_capture.h
    src/csv_writer.h
    src/recorder.h
    src/system_info.h
)

# Create executable
add_executable(recorder_core ${SOURCES} ${HEADERS})

# Windows-specific linker options
if(WIN32)
    # Link against Windows libraries
    target_link_libraries(recorder_core PRIVATE
        user32.lib
        gdi32.lib
        winmm.lib
        dxgi.lib
        d3d11.lib
        setupapi.lib
        advapi32.lib
        kernel32.lib
    )

    # Set output directory
    set_target_properties(recorder_core PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
    )

    # Ensure .exe is the output
    set_target_properties(recorder_core PROPERTIES
        OUTPUT_NAME "recorder_core"
    )
endif()

# Include directories
target_include_directories(recorder_core PRIVATE
    ${CMAKE_SOURCE_DIR}/src
    ${CMAKE_SOURCE_DIR}/third_party
    ${CMAKE_SOURCE_DIR}/third_party/nlohmann-json
    ${CMAKE_SOURCE_DIR}/third_party/readerwriterqueue
)

# OBS Studio configuration - use libobs from submodule
set(OBS_STUDIO_DIR ${CMAKE_SOURCE_DIR}/third_party/obs-studio)

if(EXISTS "${OBS_STUDIO_DIR}/libobs")
    message(STATUS "OBS Studio: Using libobs from submodule")

    # Include directories for OBS
    target_include_directories(recorder_core PRIVATE
        ${OBS_STUDIO_DIR}/libobs
        ${OBS_STUDIO_DIR}/libutil
        ${OBS_STUDIO_DIR}/deps/lzma
        ${OBS_STUDIO_DIR}/deps/zlib
        ${OBS_STUDIO_DIR}/deps/curl/include
    )

    # Find OBS library (need to build libobs first)
    # For prebuilt OBS, use the .lib files from OBS Studio distribution
    find_library(OBS_LIBRARY
        NAMES obs libobs
        PATHS ${OBS_STUDIO_DIR}/libobs/Release
              ${OBS_STUDIO_DIR}/build/libobs/Release
              ${CMAKE_SOURCE_DIR}/dist
        NO_DEFAULT_PATH
    )

    if(OBS_LIBRARY)
        message(STATUS "Found OBS library: ${OBS_LIBRARY}")
        target_link_libraries(recorder_core PRIVATE ${OBS_LIBRARY})
    else()
        message(WARNING "OBS library not found. You need to build libobs or use prebuilt OBS DLLs.")
        message(WARNING "Run the scripts in native/scripts/ to download and setup OBS DLLs.")
    endif()

    # OBS uses C++11 atomics
    if(CMAKE_VERSION VERSION_LESS "3.21")
        target_compile_options(recorder_core PRIVATE /std:c++17)
    endif()
else()
    message(WARNING "OBS Studio submodule not found at: ${OBS_STUDIO_DIR}/libobs")
    message(WARNING "Please run: git submodule update --init --recursive")
endif()

# Print configuration
message(STATUS "Recorder Core Configuration:")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Output directory: ${CMAKE_SOURCE_DIR}/dist")
