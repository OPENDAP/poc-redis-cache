cmake_minimum_required(VERSION 3.10)

project(poc_cache_tests CXX)

# -------- Config --------
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# (Optional) Enable with: -DENABLE_ASAN=ON
option(ENABLE_ASAN "Build with AddressSanitizer" OFF)

# Paths
set(PARENT_SRC_DIR "${CMAKE_CURRENT_LIST_DIR}/..")
set(TESTS_DIR      "${CMAKE_CURRENT_LIST_DIR}")

# -------- Deps: hiredis + CppUnit --------
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIB hiredis)
if (NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIB)
    message(FATAL_ERROR "hiredis not found. Set HIREDIS_INCLUDE_DIR and HIREDIS_LIB, or install hiredis.")
endif()

find_package(CppUnit REQUIRED)   # Provides CppUnit::CppUnit

# -------- Common compile flags --------
function(apply_warnings target)
    if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
        target_compile_options(${target} PRIVATE -Wall -Wextra -Wpedantic -O2)
    endif()
    if (ENABLE_ASAN)
        if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
            target_compile_options(${target} PRIVATE -fsanitize=address -fno-omit-frame-pointer)
            target_link_options(${target}    PRIVATE -fsanitize=address)
        endif()
    endif()
endfunction()

# -------- Executable: test_RedisFileCacheLRU --------
# This test needs both the cache and script manager impls.
add_executable(test_RedisFileCacheLRU
        "${TESTS_DIR}/test_RedisFileCacheLRU.cpp"
        "${PARENT_SRC_DIR}/RedisFileCacheLRU.cpp"
        "${PARENT_SRC_DIR}/ScriptManager.cpp"
)

target_include_directories(test_RedisFileCacheLRU
        PRIVATE
        "${PARENT_SRC_DIR}"        # for RedisFileCacheLRU.h, ScriptManager.h, and your main header
        "${HIREDIS_INCLUDE_DIR}"
)

target_link_libraries(test_RedisFileCacheLRU
        PRIVATE
        CppUnit::CppUnit
        "${HIREDIS_LIB}"
)

apply_warnings(test_RedisFileCacheLRU)

# -------- Executable: test_ScriptManager --------
# This test only needs ScriptManager.
add_executable(test_ScriptManager
        "${TESTS_DIR}/test_ScriptManager.cpp"
        "${PARENT_SRC_DIR}/ScriptManager.cpp"
)

target_include_directories(test_ScriptManager
        PRIVATE
        "${PARENT_SRC_DIR}"
        "${HIREDIS_INCLUDE_DIR}"
)

target_link_libraries(test_ScriptManager
        PRIVATE
        CppUnit::CppUnit
        "${HIREDIS_LIB}"
)

apply_warnings(test_ScriptManager)

# -------- Convenience output --------
message(STATUS "Built tests:")
message(STATUS "  - test_RedisFileCacheLRU")
message(STATUS "  - test_ScriptManager")
message(STATUS "Include your CppUnit main header from each test source so 'main()' is present.")
