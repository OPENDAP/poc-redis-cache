cmake_minimum_required(VERSION 3.10)
project(redis_poc_cache_cpp CXX)

set(CMAKE_CXX_STANDARD 14)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Find redis-plus-plus (assumes installed system-wide)
find_package(redis++ REQUIRED) # sometimes named "redis++_static" depending on install
find_package(hiredis REQUIRED)

add_library(redis_poc_cache
		redis_poc_cache.cpp
)
target_include_directories(redis_poc_cache PUBLIC ${CMAKE_CURRENT_SOURCE_DIR})
target_link_libraries(redis_poc_cache PUBLIC redis++ hiredis)

add_executable(test_poc_cache_mproc
		test_poc_cache_mproc.cpp
)
target_link_libraries(test_poc_cache_mproc PRIVATE redis_poc_cache redis++ hiredis)
