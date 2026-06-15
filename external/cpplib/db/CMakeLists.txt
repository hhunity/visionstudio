add_library(db SHARED logger.cpp overlay_helper.cpp)

target_compile_definitions(db PRIVATE DB_BUILD_DLL)

target_include_directories(db PUBLIC ${CMAKE_SOURCE_DIR})

target_link_libraries(db
    PUBLIC
        nlohmann_json::nlohmann_json
    PRIVATE
        unofficial::sqlite3::sqlite3
)

# Copy public header to out/<config>/include/ after build.
add_custom_command(TARGET db POST_BUILD
    COMMAND ${CMAKE_COMMAND} -E make_directory
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/include/db"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/db/logger.h"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/include/db/logger.h"
    COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/db/overlay_helper.h"
        "${CMAKE_RUNTIME_OUTPUT_DIRECTORY}/include/db/overlay_helper.h"
)
