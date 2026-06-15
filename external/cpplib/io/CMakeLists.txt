add_library(io SHARED tiff_io.cpp)

target_compile_definitions(io PRIVATE IO_BUILD_DLL)

target_include_directories(io PUBLIC ${CMAKE_SOURCE_DIR})

target_link_libraries(io
    PRIVATE
        TIFF::TIFF
        ZLIB::ZLIB
)

# Copy public headers to out/<config>/include/ after build.
add_custom_command(TARGET io POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/include/io"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/io/tiff_io.h"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/include/io/tiff_io.h"
)
