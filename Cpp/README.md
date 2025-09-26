
## Library: RedisFileCache (C++14)

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
then build redis-plus-plus (or install from your package manager if available).

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
cmake ..
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
