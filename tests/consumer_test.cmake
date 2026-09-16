execute_process(COMMAND "${CMAKE_COMMAND}"
    -S "${PLATFORM_SOURCE_DIR}/tests/consumer"
    -B "${CONSUMER_BINARY_DIR}"
    -G "${CONSUMER_GENERATOR}"
    "-DPLATFORM_SOURCE_DIR=${PLATFORM_SOURCE_DIR}"
    "-DCMAKE_BUILD_TYPE=${CONSUMER_CONFIG}"
    "-DCMAKE_CXX_COMPILER=${CONSUMER_COMPILER}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer configure failed: ${result}")
endif()

execute_process(COMMAND "${CMAKE_COMMAND}" --build "${CONSUMER_BINARY_DIR}" --config "${CONSUMER_CONFIG}"
    RESULT_VARIABLE result)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer build failed: ${result}")
endif()

set(executable "${CONSUMER_BINARY_DIR}/consumer.exe")
if(NOT EXISTS "${executable}")
    set(executable "${CONSUMER_BINARY_DIR}/${CONSUMER_CONFIG}/consumer.exe")
endif()
execute_process(COMMAND "${executable}" RESULT_VARIABLE result TIMEOUT 10)
if(NOT result EQUAL 0)
    message(FATAL_ERROR "Consumer executable failed: ${result}")
endif()
