
# Library: RedisFileCache and test code (C++14)

There are two versions of the Redis-based file cache, one that uses hiredis 
in combination with redis-plus-plus and one that uses only hiredis. I dropped
the redis++ version to eliminate a dependency. 

The hiredis code has two versions: one that is a plain copy of the Python code
also in this repo and one that has LRU cache eviction to control cache size.
The code with the LRU feature is named RedisFileCacheLRU and Test...LRU.

## About the LRU implementation

This implementation uses the Redis database to hold information about each key,
its size and its last-use time. You can look at those keys using the redis-cli.

There is an eviction log that is maintained; you can see that using the redis-cli
and the command ```LLEN``` and ```LRANGE```. Here's an example:

```redis
LRANGE poc-cache:evict:log 0 10
```

## Multi-process test: TestRedisFileCacheLRU

What it does (like the Python harness):
* Forks N worker processes (not threads).
* Each worker often reads a random key from a shared Redis SET and sometimes writes (create-only) a new file and adds its key to the set.
* Prints per-process and aggregate stats.
* Lets you dial up collisions by shortening the random suffix length.
* It uses the same Redis key set name: <namespace>:keys:set (default poc-cache:keys:set).


## Build (CMake) & Run/test

This builds both versions (LRU and plain) and the test drivers for each. This
will require that the hiredis package be installed and that it be discoverable 
by cmake.

### Build
```bash
mkdir build && cd build
cmake ..              
make -j
```

### Run tests

Here's how to run the test driver for the LRU version. The plain version can be
run the same way - the max-bytes option will be ignored - or without ```max-bytes```.
Look at the code to see all the options. 

One notable feature of the LRU test code is that setting ```processes``` to zero will
for a single process run that is easier to use in a debugger.

```bash
./TestRedisFileCacheLRU --processes 6 --duration 30 --write-prob 0.20 --key-suffix-chars 4 --max-bytes 2000000
```


### Test cleanup

The test drivers (TestRedisFileCacheLRU, ...) will remove various keys from the 
Redis database. However, you have to clean out the cache directory by hand

```bash
rm -rf /tmp/poc-cache
```
