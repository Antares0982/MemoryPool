message("Testing broken mingw thread_local")
if (NOT MSVC)
    include(CheckCXXSourceCompiles)
    try_run(test_run_tls_check # exit code
            test_compile_tls_check # boolean
            ${CMAKE_CURRENT_BINARY_DIR} # output binary
            ${CMAKE_CURRENT_SOURCE_DIR}/mingw_broken_test.cpp # source
            COMPILE_OUTPUT_VARIABLE SET_COMPILE_OUTPUT
            RUN_OUTPUT_VARIABLE SET_RUN_OUTPUT
    )

    if (NOT (test_compile_tls_check AND ("${test_run_tls_check}" EQUAL 0)))
        message(WARNING "detected broken thread_local")

        target_compile_definitions(common INTERFACE WORKAROUND_BROKEN_MINGW_TLS)
        endif ()
endif ()
