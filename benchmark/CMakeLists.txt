project(benchmark)

find_package(spdlog REQUIRED)
find_package(galay-kernel REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/galay-http)
include_directories(/usr/local/include)

# Benchmark 测试
set(BENCHMARKS
    bench_http_parser
    bench_chunked
    bench_websocket
    bench_websocket_client
    benchmark_http_router
)

foreach(BENCH_NAME ${BENCHMARKS})
    if(EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/${BENCH_NAME}.cc")
        add_executable(${BENCH_NAME} ${BENCH_NAME}.cc)
        target_link_libraries(${BENCH_NAME}
            galay-http
            pthread
        )
        target_include_directories(${BENCH_NAME} PRIVATE
            ${CMAKE_SOURCE_DIR}/galay-http
            /usr/local/include
        )
        set_target_properties(${BENCH_NAME} PROPERTIES
            CXX_STANDARD 23
            CXX_STANDARD_REQUIRED ON
            CXX_EXTENSIONS OFF
            INSTALL_RPATH $ORIGIN/../lib
        )
        list(APPEND INSTALL_BIN ${BENCH_NAME})
    endif()
endforeach()
