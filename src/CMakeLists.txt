file(GLOB_RECURSE thread_pool_SOURCES CONFIGURE_DEPENDS "*.cpp")
foreach (TMP_PATH ${thread_pool_SOURCES})
    foreach (EXCLUDE_PATH ${EXCLUDE_PATHS})
        string(FIND ${TMP_PATH} ${EXCLUDE_PATH} RESULT)
        if (NOT ${RESULT} EQUAL -1)
            list(REMOVE_ITEM thread_pool_SOURCES ${TMP_PATH})
        endif ()
    endforeach (EXCLUDE_PATH)
endforeach (TMP_PATH)

add_library(thread_pool_lib STATIC ${thread_pool_SOURCES})

target_include_directories(thread_pool_lib PUBLIC ${PROJECT_SOURCE_DIR}/includes)
