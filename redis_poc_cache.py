# redis_poc_cache.py
#
# This is a simple implementation to demonstrate how redis can be used
# to control the EFS cache in the Hyrax in the Cloud NGAP service. The
# actual cache will be a modified version of the C++ FileCache class.
#
# It's pretty easy to make a thread safe cache, a bit harder to make a
# process-safe cache, but this must be both of those and work in a multi
# host cluster.

import os
import tempfile
import uuid
from contextlib import contextmanager
from typing import Optional, BinaryIO

import redis  # conda env or pip install


class CacheBusyError(Exception):
    """Raised when a writer tries to lock while readers exist, or when a file is busy."""
    pass


class RedisFileCache:
    """
    File-backed cache with Redis-coordinated locks.
    - Single writer, multiple readers.
    - Writer lock prevents new readers; readers block writers.
    - Writer is create-only: raises FileExistsError if file already exists.
    """
    def __init__(
        self,
        cache_dir: str = "/tmp/poc-cache",
        redis_url: str = "redis://localhost:6379/0",
        lock_ttl_ms: int = 60_000,
        namespace: str = "poc-cache"
    ):
        self.cache_dir = cache_dir
        self.r = redis.from_url(redis_url, decode_responses=True)
        self.lock_ttl_ms = int(lock_ttl_ms)
        self.ns = namespace

        os.makedirs(self.cache_dir, exist_ok=True)

        # Preload Lua scripts
        self._sha_read_lock_acquire = self.r.script_load(self._lua_read_lock_acquire())
        self._sha_read_lock_release = self.r.script_load(self._lua_read_lock_release())
        self._sha_write_lock_acquire = self.r.script_load(self._lua_write_lock_acquire())
        self._sha_write_lock_release = self.r.script_load(self._lua_write_lock_release())

    # ---------- Paths & Keys ----------
    def _path(self, key: str) -> str:
        if "/" in key or key.startswith("."):
            # keep it simple for POC; adjust as needed
            raise ValueError("Key must be a simple filename without slashes or leading dot.")
        return os.path.join(self.cache_dir, key)

    def _k_write(self, key: str) -> str:
        return f"{self.ns}:lock:write:{key}"

    def _k_readers(self, key: str) -> str:
        return f"{self.ns}:lock:readers:{key}"

    # ---------- Lua scripts (atomic ops) ----------
    @staticmethod
    def _lua_read_lock_acquire() -> str:
        # KEYS[1] = write_lock, KEYS[2] = readers_count
        # ARGV[1] = ttl_ms
        return r"""
        local write_lock = KEYS[1]
        local readers = KEYS[2]
        local ttl = tonumber(ARGV[1])

        if redis.call('EXISTS', write_lock) == 1 then
            return 0
        end

        local c = redis.call('INCR', readers)
        -- (Re)set TTL each acquire to prevent stale locks if a process dies.
        redis.call('PEXPIRE', readers, ttl)
        return 1
        """

    @staticmethod
    def _lua_read_lock_release() -> str:
        # KEYS[1] = readers_count
        return r"""
        local readers = KEYS[1]
        local c = redis.call('DECR', readers)
        if c <= 0 then
            redis.call('DEL', readers)
        end
        return 1
        """

    @staticmethod
    def _lua_write_lock_acquire() -> str:
        # KEYS[1] = write_lock, KEYS[2] = readers_count
        # ARGV[1] = token, ARGV[2] = ttl_ms
        return r"""
        local write_lock = KEYS[1]
        local readers = KEYS[2]
        local token = ARGV[1]
        local ttl = tonumber(ARGV[2])

        if redis.call('EXISTS', write_lock) == 1 then
            return 0  -- another writer holds the lock
        end

        local rc = tonumber(redis.call('GET', readers) or "0")
        if rc > 0 then
            return -1  -- readers present
        end

        local ok = redis.call('SET', write_lock, token, 'NX', 'PX', ttl)
        if ok then
            return 1
        else
            return 0
        end
        """

    @staticmethod
    def _lua_write_lock_release() -> str:
        # KEYS[1] = write_lock
        # ARGV[1] = token
        return r"""
        local write_lock = KEYS[1]
        local token = ARGV[1]
        local cur = redis.call('GET', write_lock)
        if cur and cur == token then
            redis.call('DEL', write_lock)
            return 1
        end
        return 0
        """

    # ---------- Reader API ----------
    # Note that because this is a contextmanager, it returns (using 'yield')
    # an open file object to the caller that can be used as a 'generator'
    # inside a with statement. When the body of the with statement exits,
    # the remainder of the code in the generator (this function) runs.
    @contextmanager
    def open_for_read(self, key: str) -> BinaryIO:
        """
        Acquire a read lock (non-blocking). Fails if a writer holds the lock.
        Yields an open file object in 'rb' mode.
        """
        path = self._path(key)
        ok = self.r.evalsha(
            self._sha_read_lock_acquire,
            2, self._k_write(key), self._k_readers(key),
            self.lock_ttl_ms
        )
        if ok != 1:
            raise CacheBusyError(f"File '{key}' is currently being written.")

        try:
            f = open(path, "rb")
        except FileNotFoundError:
            # Release the read lock we just acquired, then re-raise
            self.r.evalsha(self._sha_read_lock_release, 1, self._k_readers(key))
            raise

        try:
            yield f
        finally:
            try:
                f.close()
            finally:
                self.r.evalsha(self._sha_read_lock_release, 1, self._k_readers(key))

    def read_bytes(self, key: str) -> bytes:
        with self.open_for_read(key) as f:
            return f.read()

    # ---------- Writer API ----------
    @contextmanager
    def open_for_create(self, key: str) -> BinaryIO:
        """
        Acquire a write lock (non-blocking) and create the file if it does not exist.
        - If file exists: raises FileExistsError.
        - If readers exist or writer exists: raises CacheBusyError.
        Yields a file object to write bytes; on exit, file is atomically moved into place and lock released.
        """
        path = self._path(key)

        # Enforce create-only semantics
        if os.path.exists(path):
            raise FileExistsError(f"File '{key}' already exists in cache.")

        token = str(uuid.uuid4())
        res = self.r.evalsha(
            self._sha_write_lock_acquire,
            2, self._k_write(key), self._k_readers(key),
            token, self.lock_ttl_ms
        )
        if res == 0:
            raise CacheBusyError(f"File '{key}' is busy (writer lock present).")
        if res == -1:
            raise CacheBusyError(f"File '{key}' is busy (readers present).")

        tmp_fd = None
        tmp_path = None
        try:
            tmp_fd, tmp_path = tempfile.mkstemp(prefix=f".{key}.", dir=self.cache_dir)
            f = os.fdopen(tmp_fd, "wb")
            tmp_fd = None  # now owned by 'f'

            # Provide a writer handle; caller writes then we finalize
            yield f

            # Ensure file flushed/closed before atomic replace
            f.flush()
            os.fsync(f.fileno())
            f.close()

            # Final check: if someone else created the file while we held the lock
            # (shouldn't happen given our write lock), enforce create-only just in case.
            if os.path.exists(path):
                raise FileExistsError(f"File '{key}' concurrently created.")

            os.replace(tmp_path, path)  # atomic
            tmp_path = None
        finally:
            # Cleanup temp file on failure
            if tmp_fd is not None:
                try:
                    os.close(tmp_fd)
                except Exception:
                    pass
            if tmp_path and os.path.exists(tmp_path):
                try:
                    os.remove(tmp_path)
                except Exception:
                    pass
            # Release writer lock
            self.r.evalsha(self._sha_write_lock_release, 1, self._k_write(key), token)

    def write_bytes_create(self, key: str, data: bytes) -> None:
        with self.open_for_create(key) as f:
            f.write(data)

    # ---------- Helpers ----------
    def exists(self, key: str) -> bool:
        return os.path.exists(self._path(key))
