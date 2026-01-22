# Example programs

# Echo Server
add_executable(echo_server echo_server.cc)
target_link_libraries(echo_server
    PRIVATE galay-http
    PRIVATE galay-kernel
)
set_target_properties(echo_server PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

# Echo Client
add_executable(echo_client echo_client.cc)
target_link_libraries(echo_client
    PRIVATE galay-http
    PRIVATE galay-kernel
)
set_target_properties(echo_client PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)

# HTTP Router Integration Example (existing)
add_executable(example_http_router_integration example_http_router_integration.cc)
target_link_libraries(example_http_router_integration
    PRIVATE galay-http
    PRIVATE galay-kernel
)
set_target_properties(example_http_router_integration PROPERTIES
    CXX_STANDARD 23
    CXX_STANDARD_REQUIRED ON
)
