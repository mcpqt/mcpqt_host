# A plugin is its own CMake target: a MODULE library (loaded at runtime by a
# host via QPluginLoader), independent of the host executable and of the
# mcp library. No "lib" prefix keeps the artifact name clean (cross_caller.so).
add_library(cross_caller MODULE cross_caller.cpp)
set_target_properties(cross_caller PROPERTIES PREFIX "")
target_link_libraries(cross_caller PRIVATE mcp::mcp)

target_compile_options(cross_caller PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >
)