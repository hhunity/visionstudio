cmake_minimum_required(VERSION 3.25)
project(cpplib LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# ---------------------------------------------------------------------------
# Output directories: all artifacts go to out/debug or out/release
# ---------------------------------------------------------------------------
string(TOLOWER "${CMAKE_BUILD_TYPE}" _build_type_lower)
set(_out_dir "${CMAKE_SOURCE_DIR}/out/${_build_type_lower}")

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY "${_out_dir}")
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY "${_out_dir}")
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY "${_out_dir}")

# ---------------------------------------------------------------------------
# Dependencies (via vcpkg)
# ---------------------------------------------------------------------------
find_package(nlohmann_json     CONFIG REQUIRED)
find_package(unofficial-sqlite3 CONFIG REQUIRED)
find_package(TIFF              REQUIRED)
find_package(ZLIB              REQUIRED)

# ---------------------------------------------------------------------------
# Subdirectories
# ---------------------------------------------------------------------------
add_subdirectory(db)
add_subdirectory(io)
add_subdirectory(test)
