# Notes plugin: SQLite-backed scratchpad. Requires system SQLite3.
find_package(SQLite3 QUIET)
if(NOT SQLite3_FOUND)
    message(STATUS "plugins/notes: SQLite3 not found; skipping notes plugin. "
                   "Install libsqlite3-dev (Linux) to build it.")
    return()
endif()

add_library(notes_plugin MODULE notes_plugin.cpp)
set_target_properties(notes_plugin PROPERTIES PREFIX "")
if(TARGET SQLite3::SQLite3)
    target_link_libraries(notes_plugin PRIVATE mcp::mcp SQLite3::SQLite3)
else()
    target_link_libraries(notes_plugin PRIVATE mcp::mcp SQLite::SQLite3)
endif()

target_compile_options(notes_plugin PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic>
)
