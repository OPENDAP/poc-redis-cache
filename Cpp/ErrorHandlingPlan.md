# Plan: Make Cache Error Sources Explicit To Clients

## Goal

Make the source of cache failures explicit to callers of `RedisFileCache`
without breaking existing behavior for lock contention, missing files, or
create-only collisions.

The immediate target is the active LRU implementation:

- [RedisFileCacheLRU.h](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU.h)
- [RedisFileCacheLRU.cpp](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU.cpp)
- [RedisFileCacheLRU_Simulator.cpp](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU_Simulator.cpp)
- [unit-tests/TestRedisFileCacheLRU.cpp](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/unit-tests/TestRedisFileCacheLRU.cpp)

## Why This Change

Today the cache exposes expected and unexpected failures through a mix of:

- `CacheBusyError`
- `std::system_error` with `ENOENT` or `EEXIST`
- generic `std::runtime_error`
- bare `bool` from blocking calls

That is enough to drive the current simulator, but it is not explicit enough
for clients that need to distinguish:

- lock contention vs timeout
- missing file vs not-yet-published file
- expected create collision vs unexpected I/O or Redis failure
- read-side unexpected failures vs write-side unexpected failures

## Recommended Direction

Implement a cache-specific error model and expose it in two layers:

1. Preserve the current API for compatibility.
2. Add richer APIs that return explicit status and failure details.

This keeps the existing code usable while giving the simulator and future
clients a better interface for diagnostics.

## Proposed Deliverables

### 1. Add cache-specific error types

Extend the public interface with typed cache exceptions that carry structured
failure information.

Suggested additions:

- `enum class CacheOperation`
- `enum class CacheErrorKind`
- `class CacheError : public std::runtime_error`
- optional derived types for common cases:
  - `CacheBusyError`
  - `CacheMissError`
  - `CacheExistsError`
  - `CacheTimeoutError`
  - `CacheIoError`
  - `CacheRedisError`

Each error should be able to report:

- operation: `read`, `write_create`, `read_blocking`, `write_blocking`, `evict`
- kind: `busy`, `missing`, `already_exists`, `timeout`, `io_error`,
  `redis_error`, `protocol_error`, `internal_error`
- optional `std::error_code`
- short stable message

### 2. Add explicit result-returning APIs

Add new methods rather than replacing the current ones.

Suggested API shape:

- `ReadResult read_bytes_ex(const std::string& key) const`
- `ReadResult read_bytes_blocking_ex(...) const`
- `WriteResult write_bytes_create_ex(...)`
- `WriteResult write_bytes_create_blocking_ex(...)`

Suggested result contents:

- `status`
- `error_kind`
- `std::error_code`
- `message`
- `data` for successful reads
- helper predicates such as `ok()`, `timed_out()`, `is_missing()`

Status values should distinguish at least:

- success
- busy
- missing
- already_exists
- timeout
- unexpected_failure

### 3. Internally normalize exceptions

Inside `RedisFileCache`, catch lower-level exceptions near the public API
boundary and translate them into cache-specific errors or result values.

Examples:

- read path:
  - writer lock held -> `busy`
  - `ENOENT` -> `missing`
  - other filesystem error -> `io_error`
  - Redis/Lua/script error -> `redis_error` or `protocol_error`
- write path:
  - readers/writer present -> `busy`
  - `EEXIST` -> `already_exists`
  - `mkstemp`/`write`/`fsync`/`rename` failure -> `io_error`
  - Redis command/script error -> `redis_error`

### 4. Keep current methods as compatibility wrappers

Do not change current behavior abruptly.

Compatibility strategy:

- existing `read_bytes()` continues to throw
- existing `write_bytes_create()` continues to throw
- existing blocking methods continue to return `bool`
- these methods become thin wrappers around the new detailed layer

That lets current users keep working while new users adopt the explicit API.

### 5. Update the simulator to use the richer API

After the detailed API exists, switch the simulator accounting from broad
catch-all buckets to explicit result inspection.

This should let the simulator report categories such as:

- read timeout waiting on busy lock
- read missing after retries
- read unexpected I/O error
- write create collision
- write timeout waiting on readers
- write unexpected Redis error

The existing summary output can be preserved, but the simulator should also log
unexpected failures with operation, kind, and message so they are diagnosable
from saved logs.

## Execution Plan

### Phase 1. Define the public error model

Work:

- add enums and `CacheError` declaration to
  [RedisFileCacheLRU.h](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU.h)
- decide whether `CacheBusyError` should remain standalone or derive from
  `CacheError`
- document the meaning of each `CacheErrorKind`

Validation:

- header remains readable and minimal
- no behavior changes yet

### Phase 2. Add detailed result structs and method declarations

Work:

- define `ReadResult` and `WriteResult` in the header
- add `*_ex` method declarations
- document success and failure semantics carefully

Validation:

- signatures are clear enough for simulator use
- compatibility methods remain unchanged

### Phase 3. Implement exception translation in the `.cpp`

Work:

- add small helpers to map `errno`, `std::system_error`, and runtime failures
  into `CacheErrorKind`
- implement the `*_ex` methods
- ensure all unexpected failures emerge with explicit operation and kind

Validation:

- reads still update LRU on success
- write semantics stay create-only
- eviction and lock behavior are unchanged

### Phase 4. Rebuild old API methods on top of the new layer

Work:

- make `read_bytes()` call `read_bytes_ex()` and translate result to the
  existing exception behavior
- make `write_bytes_create()` call `write_bytes_create_ex()`
- make blocking wrappers preserve current `bool` semantics

Validation:

- existing unit tests should still pass unchanged
- backward compatibility is preserved

### Phase 5. Strengthen unit tests

Work:

- add tests for each result status
- add tests for translation of:
  - busy read
  - missing read
  - create collision
  - blocking timeout
- add at least one test that verifies unexpected failure surfaces as an
  explicit cache-specific error or result kind

Validation:

- run the targeted C++ unit tests
- keep tests focused and deterministic

### Phase 6. Update simulator reporting

Work:

- switch the simulator to use the detailed APIs
- replace generic `other` counting with explicit subcategories where practical
- log unexpected failures with enough detail to diagnose them from the saved
  `simulator_N.log` files

Validation:

- simulator output still stays compact
- the current `ok/busy/miss` and `ok/busy/exist` summary can be preserved if
  desired
- unexpected failures become self-explanatory in logs

### Phase 7. Update documentation

Work:

- update [DeepDive.md](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/DeepDive.md)
- update [README.md](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/README.md)
- describe both the compatibility API and the detailed API

Validation:

- docs match actual method names and behavior

## Implementation Notes

### Preferred compatibility posture

The safest path is additive:

- add new types and methods
- preserve old public methods
- move internal code toward the new representation

This minimizes risk for the simulator and any existing callers.

### Preferred exception policy

Expected cache states should be easy to detect without relying on exception
message text.

That means clients should be able to identify conditions by enum or error type,
not by parsing strings such as `"open read"` or `"Unexpected reply type"`.

### Preferred simulator behavior after the change

The simulator should never again need a mystery `other` bucket for routine
analysis. If an operation fails unexpectedly, the log should say enough to tell
whether the failure came from:

- the read path or write path
- filesystem I/O
- Redis command or script execution
- a timeout
- an internal protocol or invariant failure

## Validation Plan

Primary validation flow:

```sh
cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
cmake --build build -j
ctest --test-dir build --output-on-failure
```

If `ctest` is not wired for the relevant unit tests, run the targeted cache test
binary directly from the build tree and report that clearly.

If Redis-backed tests cannot run in the current environment, validate the build,
run what is possible, and call out the gap explicitly.

## First Executable Slice

The first reviewable implementation slice should be:

1. add `CacheErrorKind`, `CacheError`, `ReadResult`, and `WriteResult`
2. add `read_bytes_ex()` and `write_bytes_create_ex()`
3. keep old methods unchanged externally
4. add unit tests for the new result types

That slice is small enough to review, immediately useful, and sets up the
blocking methods and simulator changes for a second patch.

## Out Of Scope For The First Pass

- redesigning the lock protocol
- changing Redis key layout
- changing cache eviction behavior
- changing simulator workload generation
- refactoring the retired or alternate cache implementations

## Success Criteria

This change is successful when:

- a cache client can tell exactly why an operation failed without parsing a
  generic exception string
- expected states such as busy, missing, exists, and timeout are explicit
- unexpected failures identify operation and failure kind
- existing callers can continue using the current API
- simulator logs can explain rare failures without an opaque `other` bucket
