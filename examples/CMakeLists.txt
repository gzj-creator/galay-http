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
    add_http_example(E1-EchoServer include/E1-EchoServer.cpp)
    add_http_example(E2-EchoClient include/E2-EchoClient.cpp)
    add_http_example(E3-WebsocketServer include/E3-WebsocketServer.cpp)
    add_http_example(E4-WebsocketClient include/E4-WebsocketClient.cpp)
    add_http_example(E9-H2cEchoServer include/E9-H2cEchoServer.cpp)
    add_http_example(E10-H2cEchoClient include/E10-H2cEchoClient.cpp)

    # Static file server and proxy
    add_http_example(E11-StaticServer include/E11-StaticServer.cpp)
    add_http_example(E12-HttpProxy include/E12-HttpProxy.cpp)

    # SSL echo examples
    if(GALAY_HTTP_ENABLE_SSL)
        add_http_example(E5-HttpsServer include/E5-HttpsServer.cpp)
        add_http_example(E6-HttpsClient include/E6-HttpsClient.cpp)
        add_http_example(E7-WssServer include/E7-WssServer.cpp)
        add_http_example(E8-WssClient include/E8-WssClient.cpp)
        add_http_example(E13-H2EchoServer include/E13-H2EchoServer.cpp)
        add_http_example(E14-H2EchoClient include/E14-H2EchoClient.cpp)
    endif()
endif()
