if(NOT DEFINED PROGRAM OR NOT DEFINED EXPECTED_EXIT OR NOT DEFINED EXPECTED_PATTERN)
    message(FATAL_ERROR "Missing CLI test configuration")
endif()
execute_process(COMMAND "${PROGRAM}" ${ARGUMENT}
    RESULT_VARIABLE exit_code OUTPUT_VARIABLE output ERROR_VARIABLE error
    TIMEOUT 10)
if(NOT "${exit_code}" STREQUAL "${EXPECTED_EXIT}")
    message(FATAL_ERROR "Expected exit ${EXPECTED_EXIT}, got ${exit_code}: ${output}${error}")
endif()
if(NOT "${output}${error}" MATCHES "${EXPECTED_PATTERN}")
    message(FATAL_ERROR "Missing expected output ${EXPECTED_PATTERN}: ${output}${error}")
endif()
