if(DEFINED TEST_BINARY_DIR AND DEFINED TEST_BINARY_NAME)
    set(TEST_BINARY "${TEST_BINARY_DIR}/${TEST_BINARY_NAME}")
elseif(NOT DEFINED TEST_BINARY)
    message(FATAL_ERROR "TEST_BINARY or TEST_BINARY_DIR/TEST_BINARY_NAME is required")
endif()

if(NOT DEFINED TEST_WORKING_DIRECTORY)
    message(FATAL_ERROR "TEST_WORKING_DIRECTORY is required")
endif()

if(NOT EXISTS "${TEST_BINARY}")
    message(FATAL_ERROR "Test binary does not exist: ${TEST_BINARY}")
endif()

execute_process(
    COMMAND "${TEST_BINARY}"
    WORKING_DIRECTORY "${TEST_WORKING_DIRECTORY}"
    RESULT_VARIABLE test_result
)

if(NOT test_result EQUAL 0)
    message(FATAL_ERROR "Test failed with exit code ${test_result}: ${TEST_BINARY}")
endif()
