project(test)

find_package(spdlog REQUIRED)
find_package(galay-kernel 3.4.1 REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/galay-http)

# Automatically collect all .cc files in the test directory
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

set(GALAY_HTTP_MANUAL_TEST_TARGETS
    T3-reader_writer_server
    T4-reader_writer_client
    T5-http_server
    T9-chunked_server
    T15-range_etag_server
    T18-ws_server
    T19-ws_client
    T21-https_server
    T25-h2c_client
    T26-h2c_server
    T28-h2_server
    T29-h2_client
)

# Create an executable for each .cc file
foreach(TEST_SOURCE ${TEST_SOURCES})
    # Extract the filename without path and extension
    get_filename_component(TEST_FULL_NAME ${TEST_SOURCE} NAME)
    string(REPLACE ".cc" "" TEST_NAME ${TEST_FULL_NAME})

    add_executable(${TEST_NAME} ${TEST_SOURCE})
    target_link_libraries(${TEST_NAME}
        galay-http
        pthread
    )
    if(GALAY_HTTP_COROUTINE_WORKAROUND_ENABLED)
        target_compile_options(${TEST_NAME} PRIVATE ${GALAY_HTTP_COROUTINE_WORKAROUND_FLAGS})
    endif()
    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/galay-http
    )
    target_compile_definitions(${TEST_NAME} PRIVATE USE_LOG)
    set_target_properties(${TEST_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        INSTALL_RPATH $ORIGIN/../lib
    )
    if(BUILD_TESTING)
        list(FIND GALAY_HTTP_MANUAL_TEST_TARGETS ${TEST_NAME} GALAY_HTTP_MANUAL_TEST_INDEX)
        if(GALAY_HTTP_MANUAL_TEST_INDEX EQUAL -1)
        add_test(NAME ${TEST_NAME}
            COMMAND ${CMAKE_COMMAND}
                -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DTEST_BINARY_NAME=${TEST_NAME}
                -DTEST_WORKING_DIRECTORY=${CMAKE_SOURCE_DIR}
                -P ${CMAKE_SOURCE_DIR}/cmake/RunTestBinary.cmake
        )
        endif()
    endif()
    list(APPEND INSTALL_BIN ${TEST_NAME})
endforeach()

if(BUILD_TESTING AND GALAY_HTTP_MODULE_TARGET_ENABLED)
    add_executable(T59-module_smoke T59-module_smoke.cpp)
    target_link_libraries(T59-module_smoke
        PRIVATE galay-http-modules
    )
    target_include_directories(T59-module_smoke PRIVATE
        ${CMAKE_SOURCE_DIR}/galay-http
    )
    set_target_properties(T59-module_smoke PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
    )
    if(BUILD_TESTING)
        add_test(NAME T59-module_smoke
            COMMAND ${CMAKE_COMMAND}
                -DTEST_BINARY_DIR=${CMAKE_CURRENT_BINARY_DIR}
                -DTEST_BINARY_NAME=T59-module_smoke
                -DTEST_WORKING_DIRECTORY=${CMAKE_SOURCE_DIR}
                -P ${CMAKE_SOURCE_DIR}/cmake/RunTestBinary.cmake
        )
    endif()
endif()
