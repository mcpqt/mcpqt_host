# A plugin is its own CMake target: a MODULE library (loaded at runtime by a
# host via QPluginLoader), independent of the host executable and of the
# mcp library. No "lib" prefix keeps the artifact name clean (hello_plugin.so).
add_library(hello_plugin MODULE hello_plugin.cpp)
set_target_properties(hello_plugin PROPERTIES PREFIX "")
target_link_libraries(hello_plugin PRIVATE mcp::mcp)

target_compile_options(hello_plugin PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >
)
