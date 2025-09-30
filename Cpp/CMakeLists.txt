cmake_minimum_required(VERSION 3.10)
project(poc_cache_hiredis CXX)

# C++14 (works fine for all code below)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# If hiredis isn't in a standard location, set:
#   cmake -DHiredis_ROOT=/path/to/hiredis/install ..
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIB hiredis)
if (NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIB)
	message(FATAL_ERROR "Could not find hiredis. Set HIREDIS_INCLUDE_DIR/HIREDIS_LIB or install hiredis.")
endif()

include_directories(${HIREDIS_INCLUDE_DIR})

# Library (your hiredis-based cache)
add_library(redis_poc_cache_hiredis
		redis_poc_cache_hiredis_lru.cpp
)
target_link_libraries(redis_poc_cache_hiredis
		${HIREDIS_LIB}
)

# Test / stress executable
add_executable(test_poc_cache_mproc_hiredis
		test_poc_cache_mproc_hiredis_lru.cpp
)

target_link_libraries(test_poc_cache_mproc_hiredis
		redis_poc_cache_hiredis
		${HIREDIS_LIB}
)

# Nice warnings
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(redis_poc_cache_hiredis PRIVATE -O2 -Wall -Wextra -Wpedantic)
	target_compile_options(test_poc_cache_mproc_hiredis PRIVATE -O2 -Wall -Wextra -Wpedantic)
endif()
