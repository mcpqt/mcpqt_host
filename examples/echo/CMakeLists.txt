add_library(echo_plugin MODULE echo_plugin.cpp)
set_target_properties(echo_plugin PROPERTIES PREFIX "")
target_link_libraries(echo_plugin PRIVATE mcp::mcp)

target_compile_options(echo_plugin PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic>
)