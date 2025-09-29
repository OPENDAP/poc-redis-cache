
# Library: RedisFileCache (C++14)

## hiredis version

The files with 'hiredis' in their name use only the hiredis package are easier
to build but maybe a bit less 'safe' to use.

## How to build

g++ -std=gnu++14 -O2 -Wall -Wextra -pedantic \
redis_poc_cache_hiredis.cpp test_poc_cache_mproc_hiredis.cpp \
-lhiredis -o test_poc_cache_mproc_hiredis

## How to test

./test_poc_cache_mproc_hiredis \
--processes 6 --duration 30 --write-prob 0.20 --key-suffix-chars 4

## Post-test cleanup

----

## C++ Version (which requires redis++ and did not build completely)

Features (parity with Python):
* Single-writer / multi-reader via Redis Lua scripts (atomic).
* Create-only writes (2nd writer gets std::system_error with EEXIST).
* Readers increment a Redis counter (with TTL). Writers require zero readers.
* Writers publish by writing to a temp file then rename(2) atomically.
* Keys limited to simple file names (no slashes / leading dot) for POC hygiene.

Dependencies
* redis-plus-plus (and its dependency hiredis)
* POSIX (Linux/macOS) for open, fsync, rename, etc.

Install (Ubuntu):
```bash
sudo apt-get install libhiredis-dev
```
or (OSX):
```bash
brew install hiredis
```

Build redis-plus-plus (or install from your package manager if available).
```bash
git clone https://github.com/sewenew/redis-plus-plus.git
```
For OSX and our software, I configured redis++ as:
```bash
cmake -DCMAKE_INSTALL_PREFIX=/path/to/install/redis-plus-plus -DREDIS_PLUS_PLUS_CXX_STANDARD=14 ..
```
By default, redis++ builds with C++17 and all the code that links with/against it must also be build using that version.
To avoid having conflicting versions of redis++ on my machine, I installed it in a local directory.
See the redis++ install information about other ```cmake``` switches.


## Multi-process test: test_poc_cache_mproc.cpp

What it does (like the Python harness):
* Forks N worker processes (not threads).
* Each worker often reads a random key from a shared Redis SET and sometimes writes (create-only) a new file and adds its key to the set.
* Prints per-process and aggregate stats.
* Lets you dial up collisions by shortening the random suffix length.
* It uses the same Redis key set name: <namespace>:keys:set (default poc-cache:keys:set).

## Build (CMake) & Run

### Build
```bash
mkdir build && cd build
export rpp_prefix=/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/redis-plus-plus/install/
cmake .. -DCMAKE_VERBOSE_MAKEFILE=on -DCMAKE_CXX_FLAGS=-I$rpp_prefix/include               

make -j
```

### Run tests

```bash
./test_poc_cache_mproc --processes 6 --duration 30 --write-prob 0.20 --key-suffix-chars 4
```

Like the python POC code, clean redis and /tmp/poc-cache before each trial.

### Additions

See chat for:
* Blocking acquire helpers with backoff for reads/writes.
* A size-bounded cache (LRU index + purge policy) that still respects these locks.
