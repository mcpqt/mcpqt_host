# Logging plugin: a MODULE library loaded at runtime that claims the SDK log
# sink. No "lib" prefix keeps the artifact name clean (log_plugin.so).
add_library(trivial_logger MODULE trivial_logger.cpp)
set_target_properties(trivial_logger PROPERTIES PREFIX "")
target_link_libraries(trivial_logger PRIVATE mcp::mcp)

target_compile_options(trivial_logger PRIVATE
    $<$<OR:$<CXX_COMPILER_ID:Clang>,$<CXX_COMPILER_ID:AppleClang>,$<CXX_COMPILER_ID:GNU>>:
        -Wall -Wextra -Wpedantic
    >
)
