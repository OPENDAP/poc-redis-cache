# Simulator `PID` Summary Notes

## What The Four `PID` Lines Are

The four `PID ...` lines at the top of each `simulator_N.log` file are the
exit summaries for the four worker child processes started by one simulator
instance on one node. They are not cluster-wide identities.

The simulator forks `processes` child workers in `main()`, and each child calls
`getpid()` inside `worker()` and prints one summary line when it exits. For the
March 16, 2026 run, the notes say there were 10 nodes and 4 worker processes
per node, so each `simulator_N.log` begins with four worker summaries, followed
by the parent process monitor output.

Relevant source and notes:

- [Cpp/RedisFileCacheLRU_Simulator.cpp](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU_Simulator.cpp#L114)
- [Cpp/RedisFileCacheLRU_Simulator.cpp](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU_Simulator.cpp#L208)
- [Cpp/RedisFileCacheLRU_Simulator.cpp](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU_Simulator.cpp#L430)
- [Tests/README-3.13.25](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Tests/README-3.13.25#L3)

That also explains why the same PID values appear in different log files. These
are only node-local Unix PIDs, so `3763` in one `simulator_N.log` is not the
same process as `3763` in another file.

## What The Counters Mean

The worker summary format is documented in the C++ deep dive:

```text
PID 12345 it=418 R(ok/busy/miss)=290/7/39 Rbytes=621812 W(ok/busy/exist)=82/0/0 Wbytes=171204 other=0
```

Reference:

- [Cpp/DeepDive.md](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/DeepDive.md#L261)

### Read Counters: `R(ok/busy/miss)`

- `ok`: a read succeeded and returned file contents
- `busy`: a read could not complete before the blocking timeout
- `miss`: no readable key was available

There is one important nuance in this run because the simulator was started
with `--blocking`. In blocking mode, `read_bytes_blocking()` retries both
`CacheBusyError` and transient `ENOENT` until timeout, then returns `false`.
The worker counts that `false` return as `R busy`, not `R miss`.

Relevant source:

- [Cpp/RedisFileCacheLRU_Simulator.cpp](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU_Simulator.cpp#L182)
- [Cpp/RedisFileCacheLRU.cpp](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU.cpp#L476)
- [Cpp/DeepDive.md](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/DeepDive.md#L96)

So, for this specific run:

- `R ok` means the cache file existed and could be read successfully
- `R busy` usually means the worker timed out waiting for a writer to finish,
  or retried on `ENOENT` until the timeout expired
- `R miss` mostly means `SRANDMEMBER` returned no key from the shared Redis set
  at that moment, which is most likely near startup before the cache was
  populated

### Write Counters: `W(ok/busy/exist)`

- `ok`: a create-only write succeeded
- `busy`: the writer could not get exclusive access before timeout
- `exist`: the generated key already existed, so the write hit `EEXIST`

Relevant source:

- [Cpp/RedisFileCacheLRU_Simulator.cpp](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU_Simulator.cpp#L149)
- [Cpp/RedisFileCacheLRU.cpp](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU.cpp#L288)
- [Cpp/RedisFileCacheLRU.cpp](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU.cpp#L516)

In this run:

- `W ok` means a new file was written, published into the cache, and added to
  the Redis discovery set
- `W busy` would mean lock contention with readers or another writer, but it
  did not happen in these logs
- `W exist` means the simulator generated a file name that was already present
  in the shared cache

## What The March 16, 2026 Run Suggests

Across the 40 worker summaries in `Tests/run-3.16.26-2`, the totals are:

- `R_ok=52928`
- `R_busy=632`
- `R_miss=79`
- `W_ok=9092`
- `W_busy=0`
- `W_exist=485`
- `other=1`

That suggests:

- reads usually succeeded
- read-side contention or timeout was present but fairly low
- outright misses were rare
- write lock contention was effectively absent
- the main anomaly was `W exist`

## Why `W exist` Is The Most Interesting Signal

The simulator generates a new key like this:

```text
<pid>-<hex>.bin
```

using the local process PID plus a random hex suffix.

Reference:

- [Cpp/RedisFileCacheLRU_Simulator.cpp](/Users/jimg/src/opendap/hyrax/poc-redis-cache/Cpp/RedisFileCacheLRU_Simulator.cpp#L134)

In this run, several log files reuse the same PID numbers because they came
from different nodes. Since the cache namespace was shared across all 10 nodes,
two different nodes could generate the same key if they had the same local PID
and happened to choose the same random suffix. When that happens, one writer
succeeds and the other gets `EEXIST`, which is reported as `W exist`.

That interpretation fits the logs well:

- `W busy` is zero everywhere, so lock contention is not the problem
- `W exist` spikes in `simulator_1.log` and `simulator_9.log`
- those spikes occur for PIDs such as `3763` and `3764`, which also appear in
  other node logs

## Bottom Line

The `PID` lines are per-worker summaries for the four child processes launched
on each node. In this shared-cache run, the cache locking behavior looks mostly
healthy: reads usually work, read contention is low, and writes are not timing
out on locks.

The clearest issue surfaced by these lines is not lock contention but key-name
collision in the simulator. Because the generated cache key is based on a
node-local PID plus a short random suffix, it is not globally unique enough for
multi-node shared-cache testing, and that shows up directly in the `W exist`
counts.
