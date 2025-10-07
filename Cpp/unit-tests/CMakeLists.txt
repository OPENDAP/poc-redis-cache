
# Paths
set(PARENT_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(TESTS_DIR      "${CMAKE_CURRENT_LIST_DIR}")

find_path(CPPUNIT_INCLUDE_DIR cppunit/TestFixture.h)
find_library(CPPUNIT_LIB cppunit)
if (NOT CPPUNIT_INCLUDE_DIR OR NOT CPPUNIT_LIB)
    message(FATAL_ERROR "CppUnit not found. Set CPPUNIT_INCLUDE_DIR and CPPUNIT_LIB, or install hiredis.")
endif()

message(DEBUG "Cpp/unit-test CPPUNIT_INCLUDE_DIR: ${CPPUNIT_INCLUDE_DIR}")
message(DEBUG "Cpp/unit-test CPPUNIT_LIB: ${CPPUNIT_LIB}")
message(DEBUG "Cpp/unit-test HIREDIS_INCLUDE_DIR: ${HIREDIS_INCLUDE_DIR}")
message(DEBUG "Cpp/unit-test HIREDIS_LIB: ${HIREDIS_LIB}")

# -------- Executable: test_RedisFileCacheLRU --------
# This test needs both the cache and script manager impls.
add_executable(TestRedisFileCacheLRU
        "${TESTS_DIR}/TestRedisFileCacheLRU.cpp"
        "${PARENT_SRC_DIR}/RedisFileCacheLRU.cpp"
        "${PARENT_SRC_DIR}/ScriptManager.h"
)

target_include_directories(TestRedisFileCacheLRU
        PRIVATE
        "${PARENT_SRC_DIR}"        # for RedisFileCacheLRU.h, ScriptManager.h, and your main header
        "${HIREDIS_INCLUDE_DIR}"
)

target_link_libraries(TestRedisFileCacheLRU
        PRIVATE
        "${CPPUNIT_LIB}"
        "${HIREDIS_LIB}"
)

add_test(NAME TestRedisFileCacheLRU COMMAND TestRedisFileCacheLRU)
# This enables using `ctest -L unit` to run just these tests.
set_tests_properties(TestRedisFileCacheLRU PROPERTIES LABELS unit)

# -------- Executable: test_ScriptManager --------
# This test only needs ScriptManager.
add_executable(TestScriptManager
        "${TESTS_DIR}/TestScriptManager.cpp"
        "${PARENT_SRC_DIR}/ScriptManager.h"
)

target_include_directories(TestScriptManager
        PRIVATE
        "${PARENT_SRC_DIR}"
        "${CPPUNIT_INCLUDE_DIR}"
        "${HIREDIS_INCLUDE_DIR}"
)

target_link_libraries(TestScriptManager
        PRIVATE
        "${CPPUNIT_LIB}"
        "${HIREDIS_LIB}"
)

add_test(NAME TestScriptManager COMMAND TestScriptManager)
# This enables using `ctest -L unit` to run just these tests.
set_tests_properties(TestScriptManager PROPERTIES LABELS unit)
