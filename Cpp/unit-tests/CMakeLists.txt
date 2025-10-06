# cmake_minimum_required(VERSION 3.10)

# Paths
set(PARENT_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(TESTS_DIR      "${CMAKE_CURRENT_LIST_DIR}")

find_path(CPPUNIT_INCLUDE_DIR cppunit/TestFixture.h)
find_library(CPPUNIT_LIB cppunit)
if (NOT CPPUNIT_INCLUDE_DIR OR NOT CPPUNIT_LIB)
    message(FATAL_ERROR "CppUnit not found. Set CPPUNIT_INCLUDE_DIR and CPPUNIT_LIB, or install hiredis.")
endif()

# find_package(CppUnit REQUIRED)   # Provides CppUnit::CppUnit

#if(BUILD_DEVELOPER)
#    set(DEV_FLAGS -g3 -O0 ${COMMON_WARNINGS})
#else()
#    set(DEV_FLAGS -O2 -DNDEBUG ${COMMON_WARNINGS})
#endif()
#
#if(USE_ASAN)
#    list(APPEND DEV_FLAGS -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
#    set(ASAN_LINK_FLAGS -fsanitize=address -fsanitize=undefined)
#endif()

message(STATUS "CPPUNIT_INCLUDE_DIR: ${CPPUNIT_INCLUDE_DIR}")
message(STATUS "CPPUNIT_LIB: ${CPPUNIT_LIB}")
message(STATUS "HIREDIS_INCLUDE_DIR: ${HIREDIS_INCLUDE_DIR}")
message(STATUS "HIREDIS_LIB: ${HIREDIS_LIB}")

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

#apply_warnings(TestRedisFileCacheLRU)

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

# apply_warnings(TestScriptManager)

# -------- Convenience output --------
message(STATUS "Built tests:")
message(STATUS "  - TestRedisFileCacheLRU")
message(STATUS "  - TestScriptManager")
message(STATUS "Include your CppUnit main header from each test source so 'main()' is present.")
