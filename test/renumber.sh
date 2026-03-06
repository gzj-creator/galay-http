#!/bin/bash

# 重新编号所有测试文件
mv 1.http_parser.cc 01.http_parser.cc
mv 2.chunk.cc 02.chunk.cc
mv 3.reader_writer_server.cc 03.reader_writer_server.cc
mv 4.reader_writer_client.cc 04.reader_writer_client.cc
mv 5.http_server.cc 05.http_server.cc
mv 7.http_client_awaitable.cc 06.http_client_awaitable.cc
mv 8.http_client_awaitable_edge_cases.cc 07.http_client_awaitable_edge_cases.cc
mv 9.http_methods.cc 08.http_methods.cc
mv 10.chunked_server.cc 09.chunked_server.cc
mv 11.chunked_client.cc 10.chunked_client.cc
mv 12.http_router.cc 11.http_router.cc
mv 13.http_router_validation.cc 12.http_router_validation.cc
mv 15.mount_functions.cc 13.mount_functions.cc
mv 16.static_file_transfer_modes.cc 14.static_file_transfer_modes.cc
mv 17.range_etag.cc 15.range_etag.cc
mv 20.http_client_timeout.cc 16.http_client_timeout.cc
mv 22.all_awaitable_timeout_complete.cc 17.all_awaitable_timeout_complete.cc
mv 23.websocket_frame.cc 18.websocket_frame.cc
mv 24.websocket_conn.cc 19.websocket_conn.cc
mv 25.websocket_client.cc 20.websocket_client.cc

echo "Tests renumbered successfully!"
