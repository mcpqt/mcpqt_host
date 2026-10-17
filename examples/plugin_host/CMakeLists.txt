add_executable(plugin_host main.cpp)
target_link_libraries(plugin_host PRIVATE mcp::mcp)

target_compile_options(plugin_host PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >
)
