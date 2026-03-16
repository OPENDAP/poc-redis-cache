# C++ Deep Dive

## Overview

The `Cpp/` tree is the main implementation area for the Redis-backed file cache proof of concept. The active implementation is the hiredis-based LRU cache in `RedisFileCacheLRU.*`, with:

- Redis-backed multi-process / multi-host coordination
- on-disk file storage in a shared directory
- create-only writes
- non-blocking and retrying blocking APIs
- Redis-maintained LRU metadata and bounded-size eviction

There are also two historical branches in this directory:

- `no-lru-version/`: earlier hiredis-only cache without capacity management
- `redispp-version/`: earlier `redis-plus-plus` implementation retained as a reference

## Key Files

- `RedisFileCacheLRU.h` / `RedisFileCacheLRU.cpp`: active cache implementation
- `ScriptManager.h`: lightweight Lua script registry/loader with `NOSCRIPT` recovery
- `RedisFileCacheLRU_Simulator.cpp`: multi-process stress harness
- `unit-tests/TestRedisFileCacheLRU.cpp`: behavior and eviction tests
- `unit-tests/TestScriptManager.cpp`: script manager tests
- `CMakeLists.txt`: main build for library, simulator, and tests
- `README.md`: short build/run notes
- `Design-documentation.md`: Redis key model and debugging notes

## Architecture

### Storage model

The cache stores payloads as regular files in a local or shared directory. Redis is used as a coordination and metadata plane, not as the payload store.

The active implementation separates concerns cleanly:

- filesystem: actual cached bytes
- Redis locks: read/write concurrency control
- Redis indexes: key discovery, size accounting, and LRU ordering
- Lua scripts: atomic lock state transitions

### Concurrency model

`RedisFileCache` is designed for multiple processes and multiple hosts that share the same cache directory and Redis instance. The class explicitly notes that it is not thread-safe.

Read/write coordination uses per-key Redis lock records:

- `ns:lock:write:<key>`: exclusive writer lock with TTL
- `ns:lock:readers:<key>`: reader count
- `ns:lock:evict:<key>`: short-lived eviction fence

Lua scripts enforce the lock rules atomically:

- readers fail if a writer lock exists
- writers fail if a writer exists or active readers are present
- eviction only proceeds if neither readers nor writers are active

### Metadata and LRU tracking

The LRU implementation keeps cache state in Redis:

- `ns:idx:lru` (`ZSET`): key to last-access timestamp
- `ns:idx:size` (`HASH`): key to size in bytes
- `ns:idx:total` (`STRING`): total cached bytes
- `ns:keys:set` (`SET`): all known keys, mainly for simulator discovery
- `ns:purge:mutex` (`STRING`): short-lived mutex to serialize purging
- `ns:evict:log` (`LIST`): eviction history

Writes publish a file, add size/accounting records, and optionally trigger eviction. Reads refresh the LRU score after successfully reading bytes.

## Public API

The active cache class is declared in `RedisFileCacheLRU.h` as `RedisFileCache`.

### Constructor

```cpp
RedisFileCache(
    std::string cache_dir,
    const std::string& redis_host = "127.0.0.1",
    int redis_port = 6379,
    int redis_db = 0,
    long long lock_ttl_ms = 60000,
    std::string ns = "poc-cache",
    long long max_bytes = 0);
```

Important behaviors:

- `cache_dir` is created if needed
- Redis connection is established immediately
- RESP2 is requested by `ScriptManager`
- Lua lock scripts are loaded at construction time
- `max_bytes <= 0` means unbounded cache

### Reads

- `std::string read_bytes(const std::string& key) const`
  - non-blocking
  - throws `CacheBusyError` if a writer holds the lock
  - throws `std::system_error(ENOENT)` if the file is gone
  - updates LRU on success

- `bool read_bytes_blocking(...) const`
  - retries until success or timeout
  - treats `CacheBusyError` and transient `ENOENT` as retryable
  - returns `false` on timeout

### Writes

- `void write_bytes_create(const std::string& key, const std::string& data)`
  - create-only
  - acquires write lock first
  - writes to a temp file, `fsync`s, then `rename`s into place
  - throws `EEXIST` if the target already exists
  - updates indexes and may trigger eviction

- `bool write_bytes_create_blocking(...)`
  - retries on lock conflicts
  - returns `false` on timeout

### Other helpers

- `bool exists(const std::string& key) const`
- purge tuning:
  - `set_purge_mtx_ttl(...)`
  - `set_purge_factor(...)`

## Eviction Design

Eviction is best-effort and intentionally serialized:

1. A write completes and updates total bytes.
2. If `max_bytes_ > 0` and total exceeds the cap, `ensure_capacity()` runs.
3. A purge mutex in Redis allows only one purger at a time.
4. The oldest entry in `idx:lru` is selected.
5. The implementation checks a Lua eviction fence to make sure no readers or writer are active.
6. The file is unlinked.
7. Size/LRU/key indexes are removed and the eviction is logged.

Two implementation details matter operationally:

- Purging targets `max_bytes - (max_bytes * purge_factor)`, not just `max_bytes`, to avoid immediate re-triggering.
- If writers arrive faster than purging can keep up, total bytes can temporarily exceed the configured cap.

## ScriptManager

`ScriptManager.h` is a small but important utility:

- loads Lua bodies with `SCRIPT LOAD`
- stores script name to SHA mappings
- executes via `EVALSHA`
- if Redis replies `NOSCRIPT`, reloads and retries once

That keeps the cache logic simpler and tolerates Redis script cache flushes.

## Build Layout

`CMakeLists.txt` builds:

- `redis_cache_lru` static/shared library target from `RedisFileCacheLRU.cpp`
- `RedisFileCacheLRU_Simulator` executable
- unit tests under `unit-tests/`

Build knobs:

- `BUILD_DEVELOPER=ON`: debug-friendly flags (`-g3 -O0`)
- `USE_ASAN=ON`: address and undefined behavior sanitizers

Dependencies:

- hiredis
- CppUnit for tests

## Unit Tests

The main test suite covers:

- basic write/read/index behavior
- create-only semantics
- read blocking when a writer lock is present
- retrying writer path
- retrying reader path
- LRU eviction under a small capacity

The tests use:

- unique Redis namespaces per test
- temporary cache directories under `/tmp`
- direct Redis inspection to validate index state

## Simulator Deep Dive

`RedisFileCacheLRU_Simulator.cpp` is the operational harness for stressing the cache with multiple worker processes.

### What it does

- forks worker processes, not threads
- each worker repeatedly chooses read or write based on `--write-prob`
- writes create random payload files and add them to `ns:keys:set`
- reads pick a random existing key from Redis and read from disk
- parent process prints periodic monitor output
- optional debug mode dumps Redis internals

### CLI options

The simulator currently parses these options:

| Option | Meaning | Default |
| --- | --- | --- |
| `--processes <n>` | Number of worker processes. `0` runs a single worker inline for debugging. | `4` |
| `--duration <sec>` | Worker runtime in seconds. | `20` |
| `--cache-dir <path>` | Directory used for cached files. | `/tmp/poc-cache` |
| `--redis-host <host>` | Redis hostname or IP. | `127.0.0.1` |
| `--redis-port <port>` | Redis TCP port. | `6379` |
| `--redis-db <db>` | Redis logical DB. | `0` |
| `--namespace <ns>` | Redis key namespace prefix. | `poc-cache` |
| `--clean-start` | Clear Redis state for the namespace before starting. Use only for isolated/local runs, not on every node in a shared run. | off |
| `--write-prob <p>` | Probability that an iteration is a write instead of a read. | `0.15` |
| `--read-sleep <ms>` | Sleep after each read attempt. | `5` |
| `--write-sleep <ms>` | Sleep after each write attempt. | `20` |
| `--key-suffix-chars <n>` | Random hex suffix length in new keys. Shorter values raise collision rates. | `4` |
| `--blocking` | Use the retrying read/write APIs instead of the non-blocking APIs. | off |
| `--max-bytes <n>` | Enable bounded LRU eviction with a total byte cap. `0` means unbounded. | `0` |
| `--monitor-ms <ms>` | Parent monitor interval when debug mode is off. | `1000` |
| `--debug` | Print Redis internal state during monitoring. | off |
| `--debug-interval-ms <ms>` | Monitor interval while debug mode is on. | `2000` |
| `--debug-top <n>` | Number of items shown for debug LRU/size/lock dumps. | `10` |

### Run examples

Non-blocking run:

```bash
./RedisFileCacheLRU_Simulator \
  --processes 6 \
  --duration 30 \
  --redis-host 127.0.0.1 \
  --cache-dir /tmp/poc-cache \
  --write-prob 0.20 \
  --key-suffix-chars 4
```

Blocking run with LRU cap:

```bash
./RedisFileCacheLRU_Simulator \
  --processes 6 \
  --duration 30 \
  --blocking \
  --max-bytes 2000000 \
  --monitor-ms 5000
```

Single-process debugger-friendly run:

```bash
./RedisFileCacheLRU_Simulator --processes 0 --debug
```

### Worker output

Each worker prints one summary line when it exits:

```text
PID 12345 it=418 R(ok/busy/miss)=290/7/39 Rbytes=621812 W(ok/busy/exist)=82/0/0 Wbytes=171204 other=0
```

Field meanings:

- `PID`: worker process ID
- `it`: total loop iterations
- `R(ok/busy/miss)`:
  - `ok`: successful reads
  - `busy`: read attempts blocked by lock contention or blocking timeout
  - `miss`: no key available or file missing (`ENOENT`)
- `Rbytes`: total bytes returned by successful reads
- `W(ok/busy/exist)`:
  - `ok`: successful create-only writes
  - `busy`: writes blocked by readers/writer or blocking timeout
  - `exist`: generated key already existed
- `Wbytes`: total bytes written successfully
- `other`: unexpected errors outside the expected contention/missing-file paths

### Parent monitor output

The parent prints periodic aggregate state:

```text
[monitor t=12s] total_bytes=845231 keys=311 cap=1000000
```

Field meanings:

- `t`: elapsed wall-clock seconds since worker launch
- `total_bytes`: current `ns:idx:total` value from Redis
- `keys`: current `SCARD ns:keys:set`
- `cap`: printed only when `--max-bytes > 0`

### Debug output

With `--debug`, the parent also prints:

- total cached bytes
- oldest and newest LRU entries
- sample size index entries
- recent eviction log entries
- active write locks

That makes it a practical inspection tool for lock and eviction behavior without dropping into `redis-cli`.

### Current simulator caveats

- Namespace cleanup is now opt-in via `--clean-start`; for shared multi-node runs, start from a fresh Redis/cache deployment instead of having every node clear the namespace on startup.
- The simulator expects `--redis-host` plus `--redis-port`; wrappers should pass those explicitly.

## Legacy Subdirectories

### `no-lru-version/`

This is the direct predecessor to the current implementation:

- same general filesystem plus Redis lock pattern
- no size accounting or LRU eviction
- simpler simulator and monitor output

It is useful for understanding the incremental addition of bounded-cache behavior.

### `redispp-version/`

This older branch uses `redis-plus-plus` instead of hiredis directly. It preserves the same broad ideas:

- create-only file writes
- Redis-backed reader/writer coordination
- simulator-driven testing

The top-level `Cpp/README.md` notes that this path was dropped to remove the extra dependency.

## Operational Notes

- The code assumes all participants share a cache directory path that resolves to the same backing storage, such as EFS.
- Cache keys must be simple filenames; path separators and dot-prefixed names are rejected.
- The implementation uses `steady_clock` milliseconds for LRU scoring, which is fine for ordering inside a process fleet but is not a wall-clock timestamp.
- Cleanup of cache files is separate from Redis cleanup; stale files can remain if runs are interrupted.

## Suggested Reading Order

For someone new to this code, the fastest path is:

1. `RedisFileCacheLRU.h`
2. `RedisFileCacheLRU.cpp`
3. `ScriptManager.h`
4. `unit-tests/TestRedisFileCacheLRU.cpp`
5. `RedisFileCacheLRU_Simulator.cpp`

That sequence gives the API first, then the lock/index internals, then executable examples.
