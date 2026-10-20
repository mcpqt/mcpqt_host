add_library(calculator_plugin MODULE calculator_plugin.cpp)
set_target_properties(calculator_plugin PROPERTIES PREFIX "")
target_link_libraries(calculator_plugin PRIVATE mcp::mcp)

target_compile_options(calculator_plugin PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic>
)