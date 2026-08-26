# Change Log: ESTALE read-lock race investigation

## 2026-08-25 23:xx — Initial investigation

**Prompt:**

> When we run simulator `RedisFileCacheLRU_Simulator.cpp` we see clean runs
> when running it in non-blocking mode (without the --blocking option) but
> periodic but infrequent read failures when runnint as blocking (with the
> --blocking option). These failures are reported by the simulator as 'other'
> errors but in further inspection are the result of the file descriptor used
> in the call `cache.read_bytes_blocking(key, s,
> std::chrono::milliseconds(1000))` on line 187 (in worker(...) being 'stale'
> (i.e. errno is `ESTALE`. Is this an expected error? Should Redis as the
> controller of access to the cache (which is an EFS volume in an AWS
> deployment) always prtect the open file descriptors to ensure they remain
> valid while in use by a reader?

**Proposed action (this turn):** Read-only investigation. No source files
changed. Reviewed `Cpp/RedisFileCacheLRU_Simulator.cpp`,
`Cpp/RedisFileCacheLRU.cpp`, `Cpp/RedisFileCacheLRU.h`, and
`Cpp/ErrorHandlingPlan.md` to explain the `ESTALE` failures observed only in
`--blocking` mode.

**Findings reported to user (see chat transcript for full detail):**

1. `RedisFileCache::read_bytes` (`Cpp/RedisFileCacheLRU.cpp:255-286`)
   double-releases the read lock on every failed non-blocking attempt
   (`ENOENT`/read error path calls `release_read` explicitly, then the
   catch-all `catch (...)` calls it again). Under `read_bytes_blocking`'s
   retry loop this is called many times per blocking call, prematurely
   zeroing the shared reader refcount and letting an evictor unlink a file
   while another reader still has it open.
2. `LUA_CAN_EVICT` (`Cpp/RedisFileCacheLRU.cpp:81-86`) sets an eviction
   fence key but `LUA_READ_LOCK_ACQUIRE`
   (`Cpp/RedisFileCacheLRU.cpp:62-66`) never checks it, so a reader can be
   admitted after the evictor has already decided the file is evictable.
3. EFS (NFSv4) does not preserve "unlink keeps the inode alive for open
   fds" the way local filesystems do, which is why this only surfaces as
   `ESTALE` in the AWS/EFS-backed deployment and not on local test runs, and
   why the failures are periodic/infrequent rather than constant.
4. `ESTALE` is not fully "expected" in the sense of being unavoidable, but
   given EFS/NFS semantics it should be treated as a *possible* transient
   error even after protocol fixes (filehandle expiry, remount, failover),
   and handled as a distinct, retryable error kind.

**No code changes were made in this turn.** Recommended follow-up fixes
(RAII lock/fd guards, enforcing the evict fence in the read-acquire script,
per-reader lease tokens, post-open re-validation, deferred delete via
rename-then-unlink, treating `ESTALE` as transient, and simulator keyset
hygiene) were proposed but not implemented; awaiting user decision on scope
before changing `Cpp/RedisFileCacheLRU.cpp`, `Cpp/RedisFileCacheLRU.h`, or
`Cpp/RedisFileCacheLRU_Simulator.cpp`.

## 2026-08-25 23:xx — Implement fixes 1, 2, and 7

**Prompt:**

> Yes implement 1,2 and 7. Log this action. You can use the
> @docs/estale-read-lock-race-log.md log file, just add. this prompt and a
> date-time stamp.

**Proposed action (this turn):** Implement the three approved fixes from the
prior turn's recommendation list:

1. Fix the double release/close in `RedisFileCache::read_bytes`
   (`Cpp/RedisFileCacheLRU.cpp`) — use RAII-style cleanup so the read lock
   and fd are released exactly once per call, removing the manual
   `release_read`/`close` calls from the throw paths that were also being
   hit by the trailing `catch (...)`.
2. Enforce the eviction fence — `LUA_READ_LOCK_ACQUIRE` (and
   `LUA_WRITE_LOCK_ACQUIRE`) checks `ns:lock:evict:<key>` and refuses
   admission while it is set; `try_evict_one` clears the fence explicitly
   after the unlink/index cleanup instead of relying solely on its PX TTL.
3. Simulator keyset hygiene — `Cpp/RedisFileCacheLRU_Simulator.cpp`
   `worker()` calls `SREM` on the blocking read's timed-out/`false` path
   (not just the `ENOENT` exception path) so evicted keys stop lingering in
   `ns:keys:set` and feeding retry storms.

Scope: `Cpp/RedisFileCacheLRU.cpp` and `Cpp/RedisFileCacheLRU_Simulator.cpp`
only; no header API changes expected. Will build/test per CLAUDE.md's C++
Build And Test flow where possible and report what could/could not be
validated (Redis-backed tests require an external Redis server).

**Result:**

- `Cpp/RedisFileCacheLRU.cpp`: `LUA_READ_LOCK_ACQUIRE` and
  `LUA_WRITE_LOCK_ACQUIRE` now take the evict-fence key as `KEYS[3]` and
  refuse admission (`return 0`) while it exists; `acquire_read`/
  `acquire_write` pass `k_evict_fence(key)` and `nkeys=3` accordingly.
  Added a new private `clear_evict_fence()` helper (declared in
  `Cpp/RedisFileCacheLRU.h`) that `DEL`s the fence key; `try_evict_one()`
  now calls it on every path after `can_evict_now()` has granted the
  fence, instead of relying solely on its 1500ms PX TTL. `read_bytes()`
  no longer calls `release_read()`/`close(fd)` inline on the error
  branches — cleanup now happens exactly once, in the single trailing
  `catch (...)` block.
- `Cpp/RedisFileCacheLRU_Simulator.cpp`: the blocking read's timed-out
  (`false`) branch in `worker()` now also `SREM`s the key from
  `ns:keys:set`, matching the existing `ENOENT` branch's behavior.
- Header change: added `void clear_evict_fence(const std::string& key)
  const noexcept;` to the private section of `RedisFileCacheLRU.h`. No
  public API changed.

**Validation performed:**

- Installed missing local build/runtime dependencies (`hiredis`, `redis`
  via Homebrew) and pointed CMake at a working CommandLineTools
  `clang++`/`clang` (the default Xcode.app toolchain in this environment
  was broken and unrelated to this change).
- `cmake -S . -B build -DCMAKE_BUILD_TYPE=Release ...` configured cleanly;
  `cmake --build build -j` built `redis_cache_lru`, `TestRedisFileCacheLRU`,
  `TestScriptManager`, and `RedisFileCacheLRU_Simulator` with no errors or
  warnings.
- Started a local ephemeral `redis-server` and ran
  `ctest --test-dir build --output-on-failure`: both `TestRedisFileCacheLRU`
  and `TestScriptManager` passed (100%).
- Ran `RedisFileCacheLRU_Simulator` directly against the local Redis in
  `--blocking` mode with 6 worker processes, a small `--max-bytes` cap to
  force sustained eviction churn, and short read/write sleeps: `other=0`
  for every worker across the whole run. Also ran a non-blocking-mode pass
  as a regression check: also `other=0`.
- Not validated: `ESTALE` itself is an NFS/EFS-only failure mode and
  cannot be reproduced on the local APFS filesystem used here, so this
  run confirms the lock/fence/refcount logic is correct and non-deadlocking
  under contention, but does not directly reproduce the original AWS/EFS
  failure. Recommend re-running the simulator's `--blocking` mode against
  the real EFS-backed deployment to confirm `ESTALE` no longer occurs
  there.
- Cleaned up all session-local scratch state afterward: stopped the
  ephemeral `redis-server`, removed the scratch cache directories under
  `/tmp`. `build/` was left in place (already covered by `.gitignore`).
