cmake_minimum_required(VERSION 3.10)
project(poc_cache_hiredis CXX)

# C++14 (works fine for all code below)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

option(USE_ASAN "Enable Address Sanitizer" OFF)
option(BUILD_DEVELOPER "Build in developer mode" OFF)

set(COMMON_WARNINGS -Wall -Wextra -Wpedantic)

if(BUILD_DEVELOPER)
	set(DEV_FLAGS -g3 -O0 ${COMMON_WARNINGS})
else()
	set(DEV_FLAGS -O2 -DNDEBUG ${COMMON_WARNINGS})
endif()

if(USE_ASAN)
	list(APPEND DEV_FLAGS -fsanitize=address -fsanitize=undefined -fno-omit-frame-pointer)
	set(ASAN_LINK_FLAGS -fsanitize=address -fsanitize=undefined)
endif()

# If hiredis isn't in a standard location, set:
#   cmake -DHiredis_ROOT=/path/to/hiredis/install ..
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h)
find_library(HIREDIS_LIB hiredis)
if (NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIB)
	message(FATAL_ERROR "Could not find hiredis. Set HIREDIS_INCLUDE_DIR/HIREDIS_LIB or install hiredis.")
endif()

include_directories(${HIREDIS_INCLUDE_DIR})

# Library (your hiredis-based cache)
add_library(redis_cache
		RedisFileCache.cpp
		ScriptManager.h
)
target_link_libraries(redis_cache
		${HIREDIS_LIB}
)

# Test / stress executable
add_executable(TestRedisFileCache
		TestRedisFileCache.cpp
)

target_link_libraries(TestRedisFileCache
		redis_cache
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
	target_compile_options(redis_cache_lru PRIVATE ${DEV_FLAGS})
	target_compile_options(TestRedisFileCacheLRU PRIVATE ${DEV_FLAGS})

	target_compile_options(redis_cache PRIVATE ${DEV_FLAGS})
	target_compile_options(TestRedisFileCache PRIVATE ${DEV_FLAGS})
endif()
