project(test)

find_package(spdlog REQUIRED)
find_package(galay-kernel REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/galay-http)
include_directories(/usr/local/include)

# HTTP 解析器测试
set(HTTP_TESTS
    test_http_parser
    test_chunk
    test_chunk_integration
    test_reader_writer_server
    test_reader_writer_client
    test_http_server
    test_http_client
    test_http_client_awaitable
    test_http_client_awaitable_edge_cases
    test_http_client_timeout
    test_timeout_simple
    test_socket_timeout
    test_all_awaitable_timeout_complete
    test_http_methods
    test_chunked_server
    test_chunked_client
    test_http_router
    # test_http_reader_writer  # 旧版本，已拆分为server和client
    # test_http_server_client  # 暂时注释，等HttpClient重新设计后再启用
)

# WebSocket 测试
set(WEBSOCKET_TESTS
    test_websocket_frame
    test_websocket_conn
    websocket_usage_example
)

set(ALL_TESTS ${HTTP_TESTS} ${WEBSOCKET_TESTS})

foreach(TEST_NAME ${ALL_TESTS})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${TEST_NAME}.cc")
        add_executable(${TEST_NAME} ${TEST_NAME}.cc)
        target_link_libraries(${TEST_NAME}
            galay-http
            galay-kernel::galay-kernel
            pthread
        )
        target_include_directories(${TEST_NAME} PRIVATE
            ${CMAKE_SOURCE_DIR}/galay-http
            /usr/local/include
        )
        set_target_properties(${TEST_NAME} PROPERTIES
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            INSTALL_RPATH $ORIGIN/../lib
        )
        list(APPEND INSTALL_BIN ${TEST_NAME})
    endif()
endforeach()
