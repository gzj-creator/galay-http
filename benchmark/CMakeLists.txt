project(benchmark)

find_package(spdlog REQUIRED)
find_package(galay-kernel REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/galay-http)
include_directories(/usr/local/include)

# Automatically collect all .cc files in the benchmark directory
file(GLOB BENCHMARK_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

# Create an executable for each .cc file
foreach(BENCHMARK_SOURCE ${BENCHMARK_SOURCES})
    # Extract the filename without path and extension
    get_filename_component(BENCHMARK_FULL_NAME ${BENCHMARK_SOURCE} NAME)
    string(REPLACE ".cc" "" BENCHMARK_NAME ${BENCHMARK_FULL_NAME})

    add_executable(${BENCHMARK_NAME} ${BENCHMARK_SOURCE})
    target_link_libraries(${BENCHMARK_NAME}
        galay-http
        pthread
    )
    target_include_directories(${BENCHMARK_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/galay-http
        /usr/local/include
    )
    set_target_properties(${BENCHMARK_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        INSTALL_RPATH $ORIGIN/../lib
    )
    list(APPEND INSTALL_BIN ${BENCHMARK_NAME})
endforeach()
