# Client/tooling examples. The MCP servers themselves live in ../server and
# their functionality lives in ../plugins.
if(NOT WIN32)
    add_subdirectory(simple_client)
    add_subdirectory(mcp_tester)
    add_subdirectory(hello_plugin)
    add_subdirectory(cross_caller)
    add_subdirectory(plugin_host)
    add_subdirectory(plugintester)
endif()
