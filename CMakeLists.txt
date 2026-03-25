cmake_minimum_required(VERSION 3.20)
project(visionstudio VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Fallback output directory when no preset is used
if(NOT CMAKE_RUNTIME_OUTPUT_DIRECTORY)
    string(TOLOWER "${CMAKE_BUILD_TYPE}" _build_type_lower)
    set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${CMAKE_SOURCE_DIR}/out/${_build_type_lower}")
endif()

find_package(imgui CONFIG REQUIRED)
find_package(glfw3 CONFIG REQUIRED)
find_package(TIFF REQUIRED)
find_package(ZLIB REQUIRED)
find_package(OpenGL REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(implot CONFIG REQUIRED)
find_package(httplib CONFIG REQUIRED)
find_package(nfd CONFIG REQUIRED)
find_package(Stb REQUIRED)

# ----- cpplib_io: tiff_io from external/cpplib (built as static) -----
add_library(cpplib_io STATIC
    external/cpplib/io/tiff_io.cpp
)
target_include_directories(cpplib_io PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
)
target_link_libraries(cpplib_io PUBLIC TIFF::TIFF ZLIB::ZLIB Threads::Threads)

# ----- vs_io: JSONL IO library (tiff_io now provided by cpplib_io) -----
add_library(vs_io STATIC
    io/jsonl_io.cpp
)
target_include_directories(vs_io PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
)
target_link_libraries(vs_io PUBLIC cpplib_io nlohmann_json::nlohmann_json)

# ----- vs_gui: GUI library -----
add_library(vs_gui STATIC
    gui/image_viewer.cpp
    gui/compare_viewer.cpp
)
target_include_directories(vs_gui PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
)
target_link_libraries(vs_gui PUBLIC
    imgui::imgui
    glfw
    OpenGL::GL
    glad::glad
    vs_io
    Threads::Threads
)

if(APPLE)
    target_compile_definitions(vs_gui PUBLIC GL_SILENCE_DEPRECATION)
endif()

# ----- vs_capture: HTTP capture client -----
add_library(vs_capture STATIC
    capture/capture_config.cpp
    capture/capture_client.cpp
)
target_include_directories(vs_capture PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
)
target_include_directories(vs_capture PRIVATE ${Stb_INCLUDE_DIR})
target_link_libraries(vs_capture PUBLIC
    httplib::httplib
    nlohmann_json::nlohmann_json
    Threads::Threads
)

# ----- Embed THIRD_PARTY_LICENSES.txt at build time -----
file(READ "${CMAKE_SOURCE_DIR}/THIRD_PARTY_LICENSES.txt" LICENSE_CONTENT)
configure_file(
    "${CMAKE_SOURCE_DIR}/generated/third_party_licenses.h.in"
    "${CMAKE_BINARY_DIR}/generated/third_party_licenses.h"
)

# ----- visionstudio: main application -----
add_executable(visionstudio
    api/main.cpp
)
target_include_directories(visionstudio PRIVATE ${CMAKE_BINARY_DIR})
target_link_libraries(visionstudio PRIVATE vs_gui vs_io vs_capture CLI11::CLI11 implot::implot nfd::nfd)

# ----- capture_cli: debug sandbox (opt-in, cmake -DBUILD_CAPTURE_CLI=ON) -----
option(BUILD_CAPTURE_CLI "Build capture_cli debug sandbox" OFF)
if(BUILD_CAPTURE_CLI)
    add_executable(capture_cli
        capture/capture_cli_main.cpp
    )
    target_link_libraries(capture_cli PRIVATE
        vs_capture
        CLI11::CLI11
    )
endif()

if(WIN32)
    set_target_properties(visionstudio PROPERTIES
        WIN32_EXECUTABLE $<IF:$<CONFIG:Release>,TRUE,FALSE>
    )
    target_link_options(visionstudio PRIVATE
        $<$<CONFIG:Debug>:/SUBSYSTEM:CONSOLE>
        $<$<CONFIG:Release>:/SUBSYSTEM:WINDOWS>
    )
endif()
