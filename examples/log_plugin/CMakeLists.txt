# Logging plugin: a MODULE library loaded at runtime that claims the SDK log
# sink. No "lib" prefix keeps the artifact name clean (log_plugin.so).
add_library(log_plugin MODULE log_plugin.cpp)
set_target_properties(log_plugin PROPERTIES PREFIX "")
target_link_libraries(log_plugin PRIVATE mcp::mcp)

target_compile_options(log_plugin PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >
)