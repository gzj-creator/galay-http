# Example programs

# Automatically collect all .cc files in the example directory
file(GLOB EXAMPLE_SOURCES "${CMAKE_CURRENT_SOURCE_DIR}/*.cc")

# Create an executable for each .cc file
foreach(EXAMPLE_SOURCE ${EXAMPLE_SOURCES})
    # Extract the filename without path and extension
    get_filename_component(EXAMPLE_FULL_NAME ${EXAMPLE_SOURCE} NAME)
    string(REPLACE ".cc" "" EXAMPLE_NAME ${EXAMPLE_FULL_NAME})

    add_executable(${EXAMPLE_NAME} ${EXAMPLE_SOURCE})
    target_link_libraries(${EXAMPLE_NAME}
        PRIVATE galay-http
    )
    set_target_properties(${EXAMPLE_NAME} PROPERTIES
        CXX_STANDARD 23
        CXX_STANDARD_REQUIRED ON
    )
endforeach()
