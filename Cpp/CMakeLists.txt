#cmake_minimum_required(VERSION 3.18)

# C++14 (works fine for all code below)
set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# add_subdirectory(unit-tests)

option(USE_ASAN "Enable Address Sanitizer" OFF)
option(BUILD_DEVELOPER "Build in developer mode" OFF)

# I added -Wno-c99-extensions because hiredis.h triggers that warning.
# jhrg 10/3/25
set(COMMON_WARNINGS -Wall -Wextra -Wpedantic -Wno-c99-extensions)

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
find_path(HIREDIS_INCLUDE_DIR hiredis/hiredis.h REQUIRED)
find_library(HIREDIS_LIB hiredis REQUIRED)
if (NOT HIREDIS_INCLUDE_DIR OR NOT HIREDIS_LIB)
	message(FATAL_ERROR "Could not find hiredis. Set HIREDIS_INCLUDE_DIR/HIREDIS_LIB or install hiredis.")
endif()

message(STATUS "HIREDIS_INCLUDE_DIR: ${HIREDIS_INCLUDE_DIR}")
message(STATUS "HIREDIS_LIB: ${HIREDIS_LIB}")

include_directories(${HIREDIS_INCLUDE_DIR})

#add_library(redis_cache
#		RedisFileCache.cpp
#		ScriptManager.h
#)
#target_link_libraries(redis_cache
#		${HIREDIS_LIB}
#)
#
## Test / stress executable
#add_executable(RedisFileCache_Simulator
#		RedisFileCache_Simulator.cpp
#)
#
#target_link_libraries(RedisFileCache_Simulator
#		redis_cache
#		${HIREDIS_LIB}
#)

# The same cache with LRU eviction
add_library(redis_cache_lru
		RedisFileCacheLRU.cpp
		ScriptManager.h
)
target_link_libraries(redis_cache_lru
		${HIREDIS_LIB}
)

# Test / stress executable
add_executable(RedisFileCacheLRU_Simulator
		RedisFileCacheLRU_Simulator.cpp
)

target_link_libraries(RedisFileCacheLRU_Simulator
		redis_cache_lru
		${HIREDIS_LIB}
)

# Nice warnings, debugger friendly
if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
	target_compile_options(redis_cache_lru PRIVATE ${DEV_FLAGS})
	target_compile_options(RedisFileCacheLRU_Simulator PRIVATE ${DEV_FLAGS})

#	target_compile_options(redis_cache PRIVATE ${DEV_FLAGS})
#	target_compile_options(RedisFileCache_Simulator PRIVATE ${DEV_FLAGS})
endif()

# enable_testing()
add_subdirectory(unit-tests)

## a target that runs only the unit tests:
#add_custom_target(unit-test
#		COMMAND ${CMAKE_CTEST_COMMAND} --output-on-failure -L unit
#		DEPENDS unit-tests)            # or whatever your unit‐test target is
#
## a “check” target to do unit → integration in order:
#add_custom_target(check
#		DEPENDS unit-test integration-test
#)
