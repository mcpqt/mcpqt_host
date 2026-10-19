add_executable(mcp_tester main.cpp)
target_link_libraries(mcp_tester PRIVATE mcp::mcp)

target_compile_options(mcp_tester PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >
)