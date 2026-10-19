add_executable(plugintester main.cpp)
target_link_libraries(plugintester PRIVATE mcp::mcp)

target_compile_options(plugintester PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >
)