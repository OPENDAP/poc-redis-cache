Great questions 👍 — let me walk you through the **Redis keys** this POC cache uses, what each is for, and how you can peek at them in the Redis CLI.

---

## 🔑 Redis Keys Used

All keys are prefixed by your namespace (default `poc-cache:`).

### **Locking keys (per file)**

* **`poc-cache:lock:write:<filename>`**

    * String key holding a random token while a writer owns the file.
    * Expiry (`PEXPIRE`) ensures a crashed process doesn’t leave stale locks forever.
    * Checked by both writers (to avoid double-writes) and readers (to avoid reading while a write is in-flight).

* **`poc-cache:lock:readers:<filename>`**

    * String key holding an integer count of active readers.
    * Readers `INCR` on acquire, `DECR` on release.
    * Writers refuse to lock if this is >0.

* **`poc-cache:lock:evict:<filename>`**

    * Short-lived string set when the LRU evictor is about to delete a file.
    * Prevents a new reader from sneaking in after the evictor has decided “safe to delete.”
    * Optional extra safety fence.

---

### **Index / accounting keys**

* **`poc-cache:idx:lru`** (ZSET)

    * Sorted set mapping **`member=filename`** → **`score=last access time (ms)`**.
    * Used to pick eviction victims: `ZRANGE poc-cache:idx:lru 0 0` gives the oldest entry.
    * Writers insert with current time; readers “touch” to bump recency.

* **`poc-cache:idx:size`** (HASH)

    * Hash mapping filename → file size in bytes.
    * Used for eviction accounting and for maintaining the total size counter.

* **`poc-cache:idx:total`** (STRING)

    * Integer string tracking **total bytes** of all cached files (sum of sizes).
    * Writers increment, evictor decrements.

* **`poc-cache:keys:set`** (SET)

    * All known filenames (used by the test harness so readers can pick random keys).
    * Redundant with the LRU ZSET, but convenient for tests.

* **`poc-cache:purge:mutex`** (STRING)

    * Short-TTL mutex so only one process runs `ensure_capacity()` at a time.
    * `SET NX PX` ensures only one “purger” loop is active; expires automatically.

---

## 🔍 Inspecting in Redis CLI

### Check a file’s locks

```bash
GET poc-cache:lock:write:1234-abcd.bin
GET poc-cache:lock:readers:1234-abcd.bin
```

* First shows the token if a writer is active.
* Second shows the number of readers (e.g. `"2"`).

### View LRU (ZSET)

The ZSET holds filenames as members, with last-access timestamps as scores.

* Show all with scores:

```bash
ZRANGE poc-cache:idx:lru 0 -1 WITHSCORES
```

Example output:

```
1) "1234-abcd.bin"
2) "1727823845000"
3) "5678-ef01.bin"
4) "1727823848123"
```

* `1727823845000` is the score → ms since epoch (steady_clock in our code).

* Smaller score = older (candidate for eviction).

* See the **oldest** item only:

```bash
ZRANGE poc-cache:idx:lru 0 0 WITHSCORES
```

### Check sizes

```bash
HGETALL poc-cache:idx:size
```

Outputs alternating key/value pairs:

```
1) "1234-abcd.bin"
2) "2048"
3) "5678-ef01.bin"
4) "1024"
```

### Check total bytes

```bash
GET poc-cache:idx:total
```

### Check discovery set

```bash
SMEMBERS poc-cache:keys:set
```

### Check purge mutex

```bash
GET poc-cache:purge:mutex
```

Normally empty unless an eviction loop is in progress (then contains `"1"` briefly).

---

✅ **Summary:**

* The **lock keys** enforce single-writer / multi-reader.
* The **LRU ZSET** + **size HASH** + **total counter** make eviction safe and efficient.
* The **purge mutex** keeps eviction serialized.
* You can browse them with `GET`, `ZRANGE ... WITHSCORES`, `HGETALL`, `SMEMBERS`.

---

Would it help if I added a **debug CLI command** inside the C++ program (e.g. `--debug lru 10`) that just prints the top/bottom N from `idx:lru` and current `idx:total`? That way you wouldn’t need to jump to `redis-cli` every time.
