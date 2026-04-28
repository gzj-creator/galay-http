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

function(add_http_import_example target_name source_file)
    add_executable(${target_name} ${source_file})
    target_link_libraries(${target_name} PRIVATE galay-http-modules)
    target_include_directories(${target_name} PRIVATE ${CMAKE_SOURCE_DIR})
    if(GALAY_HTTP_COROUTINE_WORKAROUND_ENABLED)
        target_compile_options(${target_name} PRIVATE ${GALAY_HTTP_COROUTINE_WORKAROUND_FLAGS})
    endif()
    set_target_properties(${target_name} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
endfunction()

if(BUILD_EXAMPLES)
    # Echo examples (HTTP / WebSocket / H2c)
    add_http_example(E1-EchoServer include/e1_echo.cpp)
    add_http_example(E2-EchoClient include/e2_echo.cpp)
    add_http_example(E3-WebsocketServer include/e3_ws.cpp)
    add_http_example(E4-WebsocketClient include/e4_ws.cpp)
    add_http_example(E9-H2cEchoServer include/e9_h2cecho.cpp)
    add_http_example(E10-H2cEchoClient include/e10_h2cecho.cpp)

    # Static file server and proxy
    add_http_example(E11-StaticServer include/e11_static.cpp)
    add_http_example(E12-HttpProxy include/e12_proxy.cpp)

    # SSL echo examples
    if(GALAY_HTTP_ENABLE_SSL)
        add_http_example(E5-HttpsServer include/e5_https.cpp)
        add_http_example(E6-HttpsClient include/e6_https.cpp)
        add_http_example(E7-WssServer include/e7_wss.cpp)
        add_http_example(E8-WssClient include/e8_wss.cpp)
        add_http_example(E13-H2EchoServer include/e13_h2echo.cpp)
        add_http_example(E14-H2EchoClient include/e14_h2echo.cpp)
    endif()
endif()

if(BUILD_EXAMPLES AND GALAY_HTTP_MODULE_TARGET_ENABLED)
    add_http_import_example(E1-EchoServerImport import/e1_echo.cpp)
    add_http_import_example(E2-EchoClientImport import/e2_echo.cpp)
    add_http_import_example(E3-WebsocketServerImport import/e3_ws.cpp)
    add_http_import_example(E4-WebsocketClientImport import/e4_ws.cpp)
    add_http_import_example(E9-H2cEchoServerImport import/e9_h2cecho.cpp)
    add_http_import_example(E10-H2cEchoClientImport import/e10_h2cecho.cpp)
    add_http_import_example(E11-StaticServerImport import/e11_static.cpp)
    add_http_import_example(E12-HttpProxyImport import/e12_proxy.cpp)

    if(GALAY_HTTP_ENABLE_SSL)
        add_http_import_example(E5-HttpsServerImport import/e5_https.cpp)
        add_http_import_example(E6-HttpsClientImport import/e6_https.cpp)
        add_http_import_example(E7-WssServerImport import/e7_wss.cpp)
        add_http_import_example(E8-WssClientImport import/e8_wss.cpp)
        add_http_import_example(E13-H2EchoServerImport import/e13_h2echo.cpp)
        add_http_import_example(E14-H2EchoClientImport import/e14_h2echo.cpp)
    endif()
endif()
