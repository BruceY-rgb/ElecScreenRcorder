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
    src/audio_device.cpp
    src/audio_capture.cpp
)

set(HEADERS
    src/protocol.h
    src/utils.h
    src/input_capture.h
    src/csv_writer.h
    src/recorder.h
    src/system_info.h
    src/audio_capture.h
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
        # WASAPI audio capture
        ole32.lib
        uuid.lib
    )

    # Set output directory (force all configs to same dir, no Release/Debug subdirectory)
    set_target_properties(recorder_core PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/dist"
        RUNTIME_OUTPUT_DIRECTORY_DEBUG "${CMAKE_SOURCE_DIR}/dist"
        RUNTIME_OUTPUT_DIRECTORY_RELEASE "${CMAKE_SOURCE_DIR}/dist"
        RUNTIME_OUTPUT_DIRECTORY_RELWITHDEBINFO "${CMAKE_SOURCE_DIR}/dist"
        RUNTIME_OUTPUT_DIRECTORY_MINSIZEREL "${CMAKE_SOURCE_DIR}/dist"
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

# FFmpeg is used as external process - no linking required
# OBS DLLs are optional (for future use)

# Print configuration
message(STATUS "Recorder Core Configuration:")
message(STATUS "  Build type: ${CMAKE_BUILD_TYPE}")
message(STATUS "  C++ standard: ${CMAKE_CXX_STANDARD}")
message(STATUS "  Output directory: ${CMAKE_SOURCE_DIR}/dist")
