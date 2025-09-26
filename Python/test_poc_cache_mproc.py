#!/usr/bin/env python3
"""
Multiprocess POC for Redis-backed file cache locking.

Spawns N processes; each process loops for D seconds, randomly choosing to
read (often) or write (sometimes) entries in /tmp/poc-cache, while using
Redis locks from RedisFileCache to enforce:
  - Single-writer
  - Multi-reader
  - Create-only writes (2nd writer gets FileExistsError)

Each successful write registers the key in a Redis SET so other processes
can discover/read it.

Usage:
  python test_poc_cache_mproc.py \
    --processes 4 \
    --duration 20 \
    --cache-dir /tmp/poc-cache \
    --redis-url redis://localhost:6379/0 \
    --namespace poc-cache \
    --write-prob 0.15 \
    --read-sleep 0.005 \
    --write-sleep 0.02
"""

import argparse
import os
import random
import string
import time
import uuid
from dataclasses import dataclass, asdict
from multiprocessing import Process, Event, Queue, current_process
from typing import Dict, Optional

import redis  # pip install redis
from redis_poc_cache import RedisFileCache, CacheBusyError  # This is the POC implementation we're testing


# ---- Test coordination keys in Redis ----
# These are keys stored in redis that enable the tests various features,
# such as, knowing which files are already in the cache (so they can be
# locked for reading).
def coord_keys(ns: str) -> Dict[str, str]:
    return {
        "set_all_keys": f"{ns}:keys:set",        # SET of cache keys known to exist
        "stats_hash": f"{ns}:test:stats",        # HSET for optional live stats (not required)
        "heartbeat": f"{ns}:test:heartbeat",     # string heartbeat (optional)
    }


# ---- Per-process stats ----
@dataclass
class Stats:
    pid: int
    proc_name: str
    reads_ok: int = 0
    reads_busy: int = 0
    reads_missing_redis: int = 0    # when a read misses, this totals the times redis did not return a key
    reads_missing_fs: int = 0       # and this totals the number of times the file system barfed. jhrg 9/23/29
    read_bytes: int = 0

    writes_ok: int = 0
    writes_busy: int = 0
    writes_exists: int = 0
    write_bytes: int = 0

    other_errors: int = 0
    iterations: int = 0

    def merge(self, other: "Stats") -> None:
        for k, v in asdict(other).items():
            if k in ("pid", "proc_name"):
                continue
            setattr(self, k, getattr(self, k) + v)


# ---- Worker loop ----
def worker_loop(
    stop_event: Event,
    result_q: Queue,
    cache_dir: str,
    redis_url: str,
    ns: str,
    write_prob: float,
    read_sleep: float,
    write_sleep: float,
) -> None:
    pid = os.getpid()
    name = current_process().name

    r = redis.from_url(redis_url, decode_responses=True)
    c = RedisFileCache(cache_dir=cache_dir, redis_url=redis_url, namespace=ns)

    coord = coord_keys(ns)
    keyset = coord["set_all_keys"]

    rnd = random.Random(pid ^ int(time.time()))
    st = Stats(pid=pid, proc_name=name)

    # Helper: make a unique new key for writes
    def new_key() -> str:
        short = uuid.uuid4().hex[:8]    # was [:8] for 8 char random names. jhrg 9/23/25
        # modify this to drop the pid and shorten the random name to increase write-write collisions
        # f"{pid}-{short}.bin". jhrg 9/23/25
        # Back up to 8. jhrg 9/23/25
        return f"{pid}-{short}.bin"

    # Helper: choose an existing key from the redis set
    def choose_existing_key() -> Optional[str]:
        return r.srandmember(keyset)

    while not stop_event.is_set():
        st.iterations += 1
        do_write = rnd.random() < write_prob

        if do_write:
            key = new_key()
            payload_len = rnd.randint(200, 4000)
            # payload includes a tiny header to help casual validation
            header = f"pid={pid};key={key};uuid={uuid.uuid4().hex}\n".encode()
            body = os.urandom(payload_len)
            data = header + body

            try:
                # create-only write
                c.write_bytes_create(key, data)
                r.sadd(keyset, key)
                st.writes_ok += 1
                st.write_bytes += len(data)
            except FileExistsError:
                st.writes_exists += 1
            except CacheBusyError:
                st.writes_busy += 1
            except Exception:
                st.other_errors += 1

            if write_sleep > 0:
                time.sleep(write_sleep)

        else:
            # READ PATH
            key = choose_existing_key()
            if not key:
                # Nothing to read yet
                st.reads_missing_redis += 1
                if read_sleep > 0:
                    time.sleep(read_sleep)
                continue

            try:
                with c.open_for_read(key) as f:
                    chunk = f.read()
                st.reads_ok += 1
                st.read_bytes += len(chunk)
            except CacheBusyError:
                # a writer currently holds the lock for that key
                st.reads_busy += 1
            except FileNotFoundError:
                # key was listed but not found (rare race or being replaced)
                st.reads_missing_fs += 1
            except Exception:
                st.other_errors += 1

            if read_sleep > 0:
                time.sleep(read_sleep)

    # send results back
    result_q.put(asdict(st))


# ---- Main harness ----
def main():
    ap = argparse.ArgumentParser(description="Multi-process Redis/Lua cache POC tester")
    ap.add_argument("--processes", type=int, default=4, help="number of worker processes")
    ap.add_argument("--duration", type=int, default=20, help="test duration in seconds")
    ap.add_argument("--cache-dir", type=str, default="/tmp/poc-cache", help="cache directory")
    ap.add_argument("--redis-url", type=str, default="redis://localhost:6379/0", help="Redis URL")
    ap.add_argument("--namespace", type=str, default="poc-cache", help="Redis namespace/prefix")
    ap.add_argument("--write-prob", type=float, default=0.15, help="probability a loop does a write")
    ap.add_argument("--read-sleep", type=float, default=0.005, help="sleep after each read (seconds)")
    ap.add_argument("--write-sleep", type=float, default=0.02, help="sleep after each write (seconds)")
    args = ap.parse_args()

    os.makedirs(args.cache_dir, exist_ok=True)

    stop_event = Event()
    result_q = Queue()

    procs = []
    for i in range(args.processes):
        p = Process(
            target=worker_loop,
            name=f"worker-{i+1}",
            args=(
                stop_event,
                result_q,
                args.cache_dir,
                args.redis_url,
                args.namespace,
                args.write_prob,
                args.read_sleep,
                args.write_sleep,
            ),
            daemon=True,
        )
        p.start()
        procs.append(p)

    # Run for duration
    t0 = time.time()
    try:
        while time.time() - t0 < args.duration:
            time.sleep(0.25)
    finally:
        stop_event.set()

    # Join workers
    for p in procs:
        p.join(timeout=5)

    # Aggregate results
    agg = Stats(pid=0, proc_name="aggregate")
    per = []
    while not result_q.empty():
        d = result_q.get()
        per.append(d)
        s = Stats(**d)
        agg.merge(s)

    # Print results
    def fmt(s: Stats) -> str:
        return (
            f"{s.proc_name} (pid={s.pid})\n"
            f"  iterations     : {s.iterations}\n"
            f"  READS  ok/busy/redis missing/fs missing : {s.reads_ok}/{s.reads_busy}/{s.reads_missing_redis}/{s.reads_missing_fs}  bytes={s.read_bytes}\n"
            f"  WRITES ok/busy/exists  : {s.writes_ok}/{s.writes_busy}/{s.writes_exists} bytes={s.write_bytes}\n"
            f"  other_errors   : {s.other_errors}\n"
        )

    print("\n=== Per-process stats ===")
    for d in per:
        print(fmt(Stats(**d)))

    print("=== Aggregate ===")
    print(fmt(agg))

    # Quick invariant checks (soft)
    if agg.writes_ok > 0 and agg.writes_exists > 0:
        print("Note: Create-only semantics exercised (writes_exits > 0).")
    if agg.reads_busy > 0 or agg.writes_busy > 0:
        print("Note: Lock contention observed (busy counters > 0).")


if __name__ == "__main__":
    main()
