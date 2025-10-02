//
// Created by James Gallagher on 9/29/25.
//

#include "redis_poc_cache_hiredis_lru.hpp"
#include "ScriptManager.h"

#include <hiredis/hiredis.h>
#include <hiredis/async.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstdarg>
#include <memory>
#include <vector>
#include <sstream>
#include <random>
#include <iomanip>
#include <thread>
#include <cerrno>

static void rc_deleter(redisContext* c) {
    if (c) redisFree(c);
}

static void ensure_dir(const std::string& d) {
    ::mkdir(d.c_str(), 0777);
}

bool RedisFileCache::file_exists_(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void RedisFileCache::fsync_fd(int fd) {
    if (::fsync(fd) != 0) {
        int e = errno;
        throw std::system_error(e, std::generic_category(), "fsync");
    }
}

void RedisFileCache::validate_key(const std::string& key) {
    if (key.empty() || key.front()=='.' || key.find('/') != std::string::npos) {
        throw std::invalid_argument("Key must be simple filename");
    }
}

std::string RedisFileCache::path_for(const std::string& key) const {
    return cache_dir_ + "/" + key;
}
std::string RedisFileCache::k_write(const std::string& key) const {
    return ns_ + ":lock:write:" + key;
}
std::string RedisFileCache::k_readers(const std::string& key) const {
    return ns_ + ":lock:readers:" + key;
}

// ------- Lua sources -------
static const char* LUA_READ_LOCK_ACQUIRE = R"(
    local wl = KEYS[1]; local rd = KEYS[2]; local ttl = tonumber(ARGV[1])
    if redis.call('EXISTS', wl) == 1 then return 0 end
    local c = redis.call('INCR', rd); redis.call('PEXPIRE', rd, ttl); return 1
)";
static const char* LUA_READ_LOCK_RELEASE = R"(
    local rd = KEYS[1]; local c = redis.call('DECR', rd)
    if c <= 0 then redis.call('DEL', rd) end; return 1
)";
static const char* LUA_WRITE_LOCK_ACQUIRE = R"(
    local wl = KEYS[1]; local rd = KEYS[2]; local token = ARGV[1]; local ttl = tonumber(ARGV[2])
    if redis.call('EXISTS', wl) == 1 then return 0 end
    local rc = tonumber(redis.call('GET', rd) or "0"); if rc > 0 then return -1 end
    local ok = redis.call('SET', wl, token, 'NX', 'PX', ttl); if ok then return 1 else return 0 end
)";
static const char* LUA_WRITE_LOCK_RELEASE = R"(
    local wl = KEYS[1]; local token = ARGV[1]; local cur = redis.call('GET', wl)
    if cur and cur == token then redis.call('DEL', wl); return 1 end; return 0
)";
static const char* LUA_CAN_EVICT = R"(
    local wl=KEYS[1]; local rd=KEYS[2]; local ev=KEYS[3]; local ttl=tonumber(ARGV[1])
    if redis.call('EXISTS', wl) == 1 then return 0 end
    local rc = tonumber(redis.call('GET', rd) or "0"); if rc > 0 then return 0 end
    local ok = redis.call('SET', ev, '1', 'NX', 'PX', ttl); if ok then return 1 else return 0 end
)";

// ------------------ LRU -----------------

long long RedisFileCache::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void RedisFileCache::touch_lru(const std::string& key, long long ts_ms) const {
    // ZADD idx:lru ts key
    cmd_ll("ZADD %s %lld %b", z_lru_.c_str(), ts_ms, key.data(), (size_t)key.size());
}

void RedisFileCache::index_add_on_publish(const std::string& key, long long size, long long ts_ms) {
    cmd_ll("HSET %s %b %lld", h_sizes_.c_str(), key.data(), (size_t)key.size(), size);
    cmd_ll("INCRBY %s %lld", k_total_.c_str(), size);
    cmd_ll("SADD %s %b", s_keys_.c_str(), key.data(), (size_t)key.size());
    touch_lru(key, ts_ms);
}

void RedisFileCache::index_remove_on_delete(const std::string& key, long long size) {
    cmd_ll("HDEL %s %b", h_sizes_.c_str(), key.data(), (size_t)key.size());
    cmd_ll("INCRBY %s %lld", k_total_.c_str(), -size);
    cmd_ll("ZREM %s %b", z_lru_.c_str(), key.data(), (size_t)key.size());
    cmd_ll("SREM %s %b", s_keys_.c_str(), key.data(), (size_t)key.size());
}

long long RedisFileCache::get_total_bytes() const {
    auto s = const_cast<RedisFileCache*>(this)->cmd_s("GET %s", k_total_.c_str());
    if (s.empty()) return 0;
    try { return std::stoll(s); } catch (...) { return 0; }
}

long long RedisFileCache::file_size_bytes(const std::string& p) {
    struct stat st{}; if (::stat(p.c_str(), &st)==0 && S_ISREG(st.st_mode)) return st.st_size;
    return 0;
}

RedisFileCache::RedisFileCache(std::string cache_dir,
                               std::string redis_host,
                               int redis_port,
                               int redis_db,
                               long long lock_ttl_ms,
                               std::string ns,
                               long long max_bytes)
: cache_dir_(std::move(cache_dir)),
  ns_(std::move(ns)),
  ttl_ms_(lock_ttl_ms),
  rc_(nullptr, rc_deleter),
  max_bytes_(max_bytes)
{
    ensure_dir(cache_dir_);

    // connect
    redisContext* c = redisConnect(redis_host.c_str(), redis_port);
    if (!c || c->err) {
        const std::string msg = c ? c->errstr : "redisConnect failed";
        if (c) redisFree(c);
        throw std::runtime_error("Redis connect error: " + msg);
    }
    rc_.reset(c);

    if (redis_db != 0) {
        cmd_ll("SELECT %d", redis_db);
    }

    // load scripts
    scripts_ = std::make_unique<ScriptManager>(rc_.get());

    scripts_->register_and_load("read_acq",  LUA_READ_LOCK_ACQUIRE);
    scripts_->register_and_load("read_rel",  LUA_READ_LOCK_RELEASE);
    scripts_->register_and_load("write_acq", LUA_WRITE_LOCK_ACQUIRE);
    scripts_->register_and_load("write_rel", LUA_WRITE_LOCK_RELEASE);
    scripts_->register_and_load("can_evict", LUA_CAN_EVICT);
}

// ------- hiredis helpers -------
long long RedisFileCache::cmd_ll(const char* fmt, ...) const {
    va_list ap; va_start(ap, fmt);
    const auto r = static_cast<redisReply *>(redisvCommand(rc_.get(), fmt, ap));
    va_end(ap);
    if (!r) throw std::runtime_error("Redis command failed (NULL reply)");
    std::unique_ptr<redisReply, void(*)(void*)> guard(r, freeReplyObject);
    if (r->type == REDIS_REPLY_INTEGER) return r->integer;
    if (r->type == REDIS_REPLY_STATUS) {
        // e.g., OK
        return 1;
    }
    if (r->type == REDIS_REPLY_NIL) return 0;
    if (r->type == REDIS_REPLY_STRING) {
        try { return std::stoll(std::string(r->str, r->len)); }
        catch (...) { throw std::runtime_error("Unexpected string reply"); }
    }
    throw std::runtime_error("Unexpected reply type (int expected)");
}

std::string RedisFileCache::cmd_s(const char* fmt, ...) const {
    va_list ap; va_start(ap, fmt);
    const auto r = static_cast<redisReply *>(redisvCommand(rc_.get(), fmt, ap));
    va_end(ap);
    if (!r) throw std::runtime_error("Redis command failed (NULL reply)");
    std::unique_ptr<redisReply, void(*)(void*)> guard(r, freeReplyObject);
    if (r->type == REDIS_REPLY_STRING || r->type == REDIS_REPLY_STATUS) {
        return {r->str, r->len};
    }
    if (r->type == REDIS_REPLY_NIL) return {};
    throw std::runtime_error("Unexpected reply type (string expected)");
}

// ------- locking -------
// read acquire
void RedisFileCache::acquire_read(const std::string& key) const {
    std::vector<std::string> KEYS{ k_write(key), k_readers(key) };
    std::vector<std::string> ARGV{ std::to_string(ttl_ms_) };
    auto res = scripts_->evalsha_ll("read_acq", 2, KEYS, ARGV);
    if (res != 1) throw CacheBusyError("read lock blocked by writer");
}

void RedisFileCache::release_read(const std::string& key) const noexcept {
    try {
        std::vector<std::string> KEYS{ k_readers(key) };
        std::vector<std::string> ARGV;
        scripts_->evalsha_ll("read_rel", 1, KEYS, ARGV);
    } catch (...) {}
}

// write acquire
std::string RedisFileCache::acquire_write(const std::string& key) const {
    // random token
    std::random_device rd; std::mt19937_64 g(rd());
    uint64_t a=g(), b=g();
    std::ostringstream oss;
    oss<<std::hex<<std::setw(16)<<std::setfill('0')<<a
       <<std::setw(16)<<std::setfill('0')<<b;
    std::string token = oss.str();

    std::vector<std::string> KEYS{ k_write(key), k_readers(key) };
    std::vector<std::string> ARGV{ token, std::to_string(ttl_ms_) };
    auto res = scripts_->evalsha_ll("write_acq", 2, KEYS, ARGV);
    if (res == 0)  throw CacheBusyError("writer lock held");
    if (res == -1) throw CacheBusyError("readers present");
    return token;
}

void RedisFileCache::release_write(const std::string& key, const std::string& token) const noexcept {
    try {
        std::vector<std::string> KEYS{ k_write(key) };
        std::vector<std::string> ARGV{ token };
        scripts_->evalsha_ll("write_rel", 1, KEYS, ARGV);
    } catch (...) {}
}

bool RedisFileCache::can_evict_now(const std::string& key) const {
    std::vector<std::string> KEYS{ k_write(key), k_readers(key), k_evict_fence(ns_, key) };
    std::vector<std::string> ARGV{ "1500" };
    auto res = scripts_->evalsha_ll("can_evict", 3, KEYS, ARGV);
    return res == 1;
}

// ------- public API -------
bool RedisFileCache::exists(const std::string& key) const {
    validate_key(key);
    return file_exists_(path_for(key));
}

std::string RedisFileCache::read_bytes(const std::string& key) {
    validate_key(key);
    auto p = path_for(key);
    acquire_read(key);
    int fd = -1;
    try {
        fd = ::open(p.c_str(), O_RDONLY);
        if (fd < 0) {
            int e = errno;
            release_read(key);
            if (e == ENOENT) throw std::system_error(e, std::generic_category(), "FileNotFound");
            throw std::system_error(e, std::generic_category(), "open read");
        }
        std::string out;
        const size_t CH=1<<16;
        char buf[CH];
        ssize_t n;
        while ((n = ::read(fd, buf, CH)) > 0) out.append(buf, buf+n);
        if (n < 0) {
            int e = errno;
            ::close(fd); release_read(key);
            throw std::system_error(e, std::generic_category(), "read");
        }
        ::close(fd); release_read(key);
        touch_lru(key, now_ms());
        return out;
    } catch (...) {
        if (fd >= 0) ::close(fd);
        release_read(key);
        throw;
    }
}

void RedisFileCache::write_bytes_create(const std::string& key, const std::string& data) {
    validate_key(key);
    auto p = path_for(key);
    if (file_exists_(p)) throw std::system_error(EEXIST, std::generic_category(), "exists");

    auto token = acquire_write(key);

    // tmp file
    char tmpl[4096];
    std::snprintf(tmpl, sizeof(tmpl), "%s/.%s.XXXXXX", cache_dir_.c_str(), key.c_str());
    int tfd = ::mkstemp(tmpl);
    if (tfd < 0) {
        int e = errno; release_write(key, token);
        throw std::system_error(e, std::generic_category(), "mkstemp");
    }

    // write data
    ssize_t left = (ssize_t)data.size();
    const char* ptr = data.data();
    ssize_t wrote = 0;
    while (left > 0) {
        ssize_t n = ::write(tfd, ptr + wrote, left);
        if (n < 0) {
            int e = errno; ::close(tfd); ::unlink(tmpl); release_write(key, token);
            throw std::system_error(e, std::generic_category(), "write");
        }
        wrote += n; left -= n;
    }
    try { fsync_fd(tfd); } catch (...) {
        ::close(tfd); ::unlink(tmpl); release_write(key, token); throw;
    }
    ::close(tfd);

    // final create-only check (belt & suspenders)
    if (file_exists_(p)) {
        ::unlink(tmpl); release_write(key, token);
        throw std::system_error(EEXIST, std::generic_category(), "concurrent create");
    }

    if (::rename(tmpl, p.c_str()) != 0) {
        int e = errno; ::unlink(tmpl); release_write(key, token);
        throw std::system_error(e, std::generic_category(), "rename");
    }

    release_write(key, token);

    // ----- NEW: record size + touch LRU + enforce capacity -----
    auto sz = (long long)data.size();
    long long ts = now_ms();
    index_add_on_publish(key, sz, ts);

    if (max_bytes_ > 0) {
        ensure_capacity(); // best-effort purge loop
    }
}

void RedisFileCache::ensure_capacity() {
    if (max_bytes_ <= 0) return;

    // best-effort single purger: SET NX PX 2s
    // if this fails, another process is purging; return
    auto ok = cmd_s("SET %s 1 NX PX %d", k_purge_mtx_.c_str(), 2000);
    if (ok != "OK") return;

    try {
        while (get_total_bytes() > max_bytes_) {
            std::string victim;
            long long freed = 0;
            if (!try_evict_one(victim, freed)) break;
        }
    } catch (...) {
        // swallow; purger is best-effort
    }
    // mutex auto-expires
}

/**
 * Try to evict one file from the cache. This method chooses the victim based
 * on the LRU data stored in the Redis server. If successful, it will return
 * the name and size of the 'victim' using the two value-result parameters.
 *
 * @param victim Name of the file removed
 * @param freed number of bytes removed from teh cache
 * @return true if a file was removed, false otherwise.
 */
bool RedisFileCache::try_evict_one(std::string& victim, long long& freed) {
    victim.clear(); freed = 0;

    // Oldest (lowest score) by LRU
    const auto r = static_cast<redisReply *>(redisCommand(rc_.get(), "ZRANGE %s 0 0 WITHSCORES", z_lru_.c_str()));
    if (!r) return false;
    std::unique_ptr<redisReply, void(*)(void*)> guard(r, freeReplyObject);

    if (r->type != REDIS_REPLY_ARRAY || r->elements < 1) return false;

    // This is the key of the file to be removed (i.e., with the lowest LRU score)
    const std::string key(r->element[0]->str, r->element[0]->len);

    // size lookup; see 'sz' below
    const auto rs = static_cast<redisReply *>(redisCommand(rc_.get(), "HGET %s %b", h_sizes_.c_str(),
        key.data(), (size_t) key.size()));
    if (!rs) return false;
    std::unique_ptr<redisReply, void(*)(void*)> guards(rs, freeReplyObject);

    if (rs->type == REDIS_REPLY_NIL) {
        // index drift; clean LRU entry and continue
        cmd_ll("ZREM %s %b", z_lru_.c_str(), key.data(), (size_t)key.size());
        cmd_ll("SREM %s %b", s_keys_.c_str(), key.data(), (size_t)key.size());
        return false;
    }
    // This is the size of the file to remove.
    const auto sz = std::stoll(std::string(rs->str, rs->len));

    // Fence & verify evictable (no readers/writers)
    if (!can_evict_now(key)) {
        // Nudge LRU to avoid hammering
        touch_lru(key, now_ms());
        return false;
    }

    // Remove from FS
    const auto p = path_for(key);
    if (::unlink(p.c_str()) != 0) {
        // file already gone? clean indexes
        index_remove_on_delete(key, sz);
        return false;
    }

    // Clean indexes
    index_remove_on_delete(key, sz);
    victim = key;
    freed = sz;
    return true;
}

/**
  * Read cached bytes for a key, waiting for an in‑progress writer to finish.
  *
  * This is a convenience wrapper around read_bytes() that handles the case
  * where a concurrent writer holds the write lock. When the underlying read
  * would fail with CacheBusyError (i.e., a writer is present), this method
  * will repeatedly retry until either the read succeeds or the timeout is
  * reached. Between retries, it sleeps for the specified backoff duration.
  *
  * Thread-safety and process-safety are managed via Redis-based locks; this
  * call does not itself hold the write lock and will not block other readers.
  *
  * @param key      Cache key to read. Must be a valid cache key accepted by
  *                 validate_key().
  * @param out      On success, filled with the value read from the file cache.
  *                 On failure, its content is unspecified.
  * @param timeout  Maximum time to wait for the value to become readable. A
  *                 zero or negative duration performs a single non-blocking
  *                 attempt.
  * @param backoff  Sleep duration between retries while the writer is active.
  *                 Defaults to 10ms. Must be non-negative.
  * @return true if the value was read successfully within the timeout; false
  *         if the key does not exist by the timeout or another retriable
  *         condition prevented a successful read within the allotted time.
  * @throws std::system_error on non-retriable filesystem or Redis errors
  *         encountered during read attempts. In particular, ENOENT from the
  *         final attempt is treated as a non-exceptional false return when it
  *         represents a missing key rather than a transient condition.
  */
bool RedisFileCache::read_bytes_blocking(const std::string& key,
                                         std::string& out,
                                         std::chrono::milliseconds timeout,
                                         std::chrono::milliseconds backoff)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        try {
            out = read_bytes(key);           // non-blocking attempt
            return true;
        } catch (const CacheBusyError&) {
            // writer present: retry
        } catch (const std::system_error& se) {
            if (se.code().value() == ENOENT) {
                // not yet published: retry
            } else {
                throw; // real error
            }
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(backoff);
    }
}

/**
 * Write bytes for a key if absent, waiting for conflicting readers/writers.
 *
 * This method attempts write_bytes_create(), and if a conflicting reader or
 * writer holds the lock, it will retry until success or until the timeout is
 * reached, sleeping for the given backoff between attempts.
 *
 * @param key      Cache key to create and write.
 * @param data     Data to persist for the key.
 * @param timeout  Maximum time to wait for exclusive access to perform the
 *                 create + write operation.
 * @param backoff  Sleep duration between retries. Defaults to 10ms.
 * @return true if the write operation succeeded within the timeout; false if
 *          it timed out without completing.
 * @throws std::system_error on non-retriable filesystem or Redis errors.
 */
bool RedisFileCache::write_bytes_create_blocking(const std::string& key,
                                                 const std::string& data,
                                                 std::chrono::milliseconds timeout,
                                                 std::chrono::milliseconds backoff)
{
    auto deadline = std::chrono::steady_clock::now() + timeout;
    while (true) {
        try {
            write_bytes_create(key, data);   // non-blocking path
            return true;
        } catch (const CacheBusyError&) {
            // writer/readers present: retry
        } catch (const std::system_error& se) {
            if (se.code().value() == EEXIST) throw; // permanent condition
            throw; // other error
        }
        if (std::chrono::steady_clock::now() >= deadline) return false;
        std::this_thread::sleep_for(backoff);
    }
}
