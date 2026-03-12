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
        Shcore.lib
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

# OBS Studio configuration - use prebuilt OBS from dist directory
set(OBS_BIN_DIR ${CMAKE_SOURCE_DIR}/dist)
set(OBS_INCLUDE_DIRS
    "${CMAKE_SOURCE_DIR}/third_party/obs-studio/obs-studio-master/libobs"
    "${CMAKE_SOURCE_DIR}/third_party/obs-studio/obs-studio-master/libobs/util"
)

if(EXISTS "${OBS_BIN_DIR}/obs.dll")
    message(STATUS "OBS Studio: Using prebuilt OBS from obs-download")

    # Include directories for OBS
    target_include_directories(recorder_core PRIVATE ${OBS_INCLUDE_DIRS})

    # Add OBS DLL directory to runtime path
    target_link_directories(recorder_core PRIVATE ${OBS_BIN_DIR})

    # Link against OBS DLL (using delay loading to avoid needing obs.lib)
    target_link_libraries(recorder_core PRIVATE delayimp.lib)
    set_target_properties(recorder_core PROPERTIES
        LINK_FLAGS "/DELAYLOAD:obs.dll"
    )

    # Copy OBS DLLs to output directory
    file(GLOB OBS_DLLS ${OBS_BIN_DIR}/*.dll)
    foreach(DLL ${OBS_DLLS})
        add_custom_command(TARGET recorder_core POST_BUILD
            COMMAND ${CMAKE_COMMAND} -E copy_if_different
            ${DLL}
            $<TARGET_FILE_DIR:recorder_core>
        )
    endforeach()

    # OBS uses C++11 atomics
    if(CMAKE_VERSION VERSION_LESS "3.21")
        target_compile_options(recorder_core PRIVATE /std:c++17)
    endif()
else()
    message(FATAL_ERROR "OBS DLL not found. Please run scripts/download-obs.ps1 to download OBS Studio.")
endif()

# Print configuration
message(STATUS "Recorder Core Configuration:")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Output directory: ${CMAKE_SOURCE_DIR}/dist")
