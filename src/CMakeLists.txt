target_sources(mcp PRIVATE
    client.cpp
    log.cpp
    plugin.cpp
    plugin_manager.cpp
    protocol.cpp
    server.cpp
    session.cpp
    # Headers are listed so IDEs (Qt Creator, CLion, VS) show them.
    client.hpp
    error.hpp
    json.hpp
    log.hpp
    mcp.hpp
    plugin.hpp
    plugin_manager.hpp
    protocol.hpp
    server.hpp
    session.hpp
    transport.hpp
)

# POSIX-only (poll(2) + fds). Windows consumers use the HTTP transport;
# the header carries a matching #error guard.
if(NOT WIN32)
    target_sources(mcp PRIVATE stdio_transport.cpp stdio_transport.hpp)
endif()

# Optional HTTP transport, split into its own target so that binaries that
# only speak stdio (and therefore never touch QtNetwork/QtHttpServer) link
# nothing beyond Qt6::Core. Consumers link `mcp::http` (which brings in `mcp`).
if(MCP_ENABLE_HTTP)
    add_library(mcp-http
        http_client_transport.cpp
        http_client_transport.hpp
        http_server_host.cpp
        http_server_host.hpp
    )
    add_library(mcp::http ALIAS mcp-http)
    # Export as `mcp::http` (not `mcp::mcp-http`) so installed consumers use the
    # same name as the in-tree alias. install(EXPORT) ignores ALIAS targets and
    # would otherwise namespace the raw target name `mcp-http`.
    set_target_properties(mcp-http PROPERTIES EXPORT_NAME http)

    target_link_libraries(mcp-http PUBLIC mcp Qt6::Network Qt6::HttpServer)
    # PUBLIC so the `#if !defined(MCP_ENABLE_HTTP)` guards in the HTTP headers
    # are satisfied for anyone linking mcp::http.
    target_compile_definitions(mcp-http PUBLIC MCP_ENABLE_HTTP=1)

    mcp_target_warnings(mcp-http)
endif()
