# Starts the mock CrossPoint server, runs the upload test, stops the server.
file(REMOVE_RECURSE ${ROOT})
execute_process(COMMAND sh -c "${PY} ${MOCK} 8765 ${ROOT} >/dev/null 2>&1 & echo $!" OUTPUT_VARIABLE PID
    OUTPUT_STRIP_TRAILING_WHITESPACE)
execute_process(COMMAND sleep 1)
execute_process(COMMAND ${TEST} http://127.0.0.1:8765/ ${EPUB} RESULT_VARIABLE R)
execute_process(COMMAND kill ${PID})
if(NOT R EQUAL 0)
    message(FATAL_ERROR "crosspoint test failed (${R})")
endif()
