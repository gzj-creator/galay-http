function(add_http_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay-http)
    target_include_directories(${target_name} PRIVATE ${CMAKE_SOURCE_DIR})
    if(GALAY_HTTP_COROUTINE_WORKAROUND_ENABLED)
        target_compile_options(${target_name} PRIVATE ${GALAY_HTTP_COROUTINE_WORKAROUND_FLAGS})
    endif()
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
    )
endfunction()

if(BUILD_EXAMPLES)
    # Echo examples (HTTP / WebSocket / H2c)
    add_http_example(E1-EchoServer include/E1-echo_server.cpp)
    add_http_example(E2-EchoClient include/E2-echo_client.cpp)
    add_http_example(E3-WebsocketServer include/E3-websocket_server.cpp)
    add_http_example(E4-WebsocketClient include/E4-websocket_client.cpp)
    add_http_example(E9-H2cEchoServer include/E9-h2c_echo_server.cpp)
    add_http_example(E10-H2cEchoClient include/E10-h2c_echo_client.cpp)

    # Static file server and proxy
    add_http_example(E11-StaticServer include/E11-static_server.cpp)
    add_http_example(E12-HttpProxy include/E12-http_proxy.cpp)

    # SSL echo examples
    if(GALAY_HTTP_ENABLE_SSL)
        add_http_example(E5-HttpsServer include/E5-https_server.cpp)
        add_http_example(E6-HttpsClient include/E6-https_client.cpp)
        add_http_example(E7-WssServer include/E7-wss_server.cpp)
        add_http_example(E8-WssClient include/E8-wss_client.cpp)
        add_http_example(E13-H2EchoServer include/E13-h2_echo_server.cpp)
        add_http_example(E14-H2EchoClient include/E14-h2_echo_client.cpp)
    endif()
endif()
