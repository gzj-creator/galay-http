project(test)

find_package(spdlog REQUIRED)
find_package(galay-kernel REQUIRED)

include_directories(${CMAKE_SOURCE_DIR}/galay-http)
include_directories(/usr/local/include)

# Automatically collect all .cc files in the test directory
file(GLOB TEST_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

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
    target_include_directories(${TEST_NAME} PRIVATE
        ${CMAKE_SOURCE_DIR}/galay-http
        /usr/local/include
    )
    target_compile_definitions(${TEST_NAME} PRIVATE USE_LOG)
    set_target_properties(${TEST_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
        CXX_EXTENSIONS OFF
        INSTALL_RPATH $ORIGIN/../lib
    )
    list(APPEND INSTALL_BIN ${TEST_NAME})
endforeach()
