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
		redis_poc_cache_hiredis.cpp
		ScriptManager.h
)
target_link_libraries(redis_poc_cache_hiredis
		${HIREDIS_LIB}
)

# Test / stress executable
add_executable(test_poc_cache_mproc_hiredis
		test_poc_cache_mproc_hiredis.cpp
)

target_link_libraries(test_poc_cache_mproc_hiredis
		redis_poc_cache_hiredis
		${HIREDIS_LIB}
)

# The same cache with LRU eviction
add_library(redis_cache_lru
		RedisFileCacheLRU.cpp
		ScriptManager.h
)
target_link_libraries(redis_cache_lru
		${HIREDIS_LIB}
)

# Test / stress executable
add_executable(TestRedisFileCacheLRU
		TestRedisFileCacheLRU.cpp
)

target_link_libraries(TestRedisFileCacheLRU
		redis_cache_lru
		${HIREDIS_LIB}
)

# Nice warnings, debugger friendly
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(redis_cache_lru PRIVATE -O0 -g3 -Wall -Wextra -Wpedantic)
	target_compile_options(TestRedisFileCacheLRU PRIVATE -O0 -g3 -Wall -Wextra -Wpedantic)
	target_compile_options(redis_poc_cache_hiredis PRIVATE -O0 -g3 -Wall -Wextra -Wpedantic)
	target_compile_options(test_poc_cache_mproc_hiredis PRIVATE -O0 -g3 -Wall -Wextra -Wpedantic)

endif()
