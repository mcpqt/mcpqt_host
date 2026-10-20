# MCP server front-ends. These are the project's primary targets, not examples.

if(NOT WIN32)
    add_executable(mcp_stdio_server stdio_server.cpp)
    target_link_libraries(mcp_stdio_server PRIVATE mcp::mcp)
    target_compile_options(mcp_stdio_server PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
            -Wall -Wextra -Wpedantic>
    )
endif()

if(MCP_ENABLE_HTTP)
    add_executable(mcp_http_server http_server.cpp)
    target_link_libraries(mcp_http_server PRIVATE mcp::http)
    target_compile_options(mcp_http_server PRIVATE
        $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
            -Wall -Wextra -Wpedantic>
    )
endif()