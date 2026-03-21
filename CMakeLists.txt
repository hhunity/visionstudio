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
find_package(OpenGL REQUIRED)
find_package(glad CONFIG REQUIRED)
find_package(Threads REQUIRED)
find_package(CLI11 CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)

# ----- vs_io: TIFF IO + JSONL IO library -----
add_library(vs_io STATIC
    io/tiff_io.cpp
    io/jsonl_io.cpp
)
target_include_directories(vs_io PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}>
)
target_link_libraries(vs_io PUBLIC TIFF::TIFF Threads::Threads nlohmann_json::nlohmann_json)

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

# ----- visionstudio: main application -----
add_executable(visionstudio
    api/main.cpp
)
target_link_libraries(visionstudio PRIVATE vs_gui vs_io CLI11::CLI11)
