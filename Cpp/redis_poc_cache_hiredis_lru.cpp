//
// Created by James Gallagher on 9/29/25.
//

#include "redis_poc_cache_hiredis_lru.hpp"

#include <hiredis/hiredis.h>
#include <hiredis/async.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>

#include <cstring>
#include <cstdarg>
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
const char* RedisFileCache::LUA_READ_LOCK_ACQUIRE() {
    return R"(
        local wl = KEYS[1]
        local rd = KEYS[2]
        local ttl = tonumber(ARGV[1])
        if redis.call('EXISTS', wl) == 1 then return 0 end
        local c = redis.call('INCR', rd)
        redis.call('PEXPIRE', rd, ttl)
        return 1
    )";
}
const char* RedisFileCache::LUA_READ_LOCK_RELEASE() {
    return R"(
        local rd = KEYS[1]
        local c = redis.call('DECR', rd)
        if c <= 0 then redis.call('DEL', rd) end
        return 1
    )";
}
const char* RedisFileCache::LUA_WRITE_LOCK_ACQUIRE() {
    return R"(
        local wl = KEYS[1]
        local rd = KEYS[2]
        local token = ARGV[1]
        local ttl = tonumber(ARGV[2])
        if redis.call('EXISTS', wl) == 1 then return 0 end
        local rc = tonumber(redis.call('GET', rd) or "0")
        if rc > 0 then return -1 end
        local ok = redis.call('SET', wl, token, 'NX', 'PX', ttl)
        if ok then return 1 else return 0 end
    )";
}
const char* RedisFileCache::LUA_WRITE_LOCK_RELEASE() {
    return R"(
        local wl = KEYS[1]
        local token = ARGV[1]
        local cur = redis.call('GET', wl)
        if cur and cur == token then
            redis.call('DEL', wl)
            return 1
        end
        return 0
    )";
}

// ------------------ LRU -----------------

std::string RedisFileCache::k_evict_fence(const std::string& ns, const std::string& key) {
    return ns + ":lock:evict:" + key;
}

long long RedisFileCache::now_ms() {
    using namespace std::chrono;
    return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

void RedisFileCache::touch_lru(const std::string& key, long long ts_ms) {
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

long long RedisFileCache::file_size_bytes(const std::string& p) const {
    struct stat st{}; if (::stat(p.c_str(), &st)==0 && S_ISREG(st.st_mode)) return st.st_size;
    return 0;
}

// ----------------- lua for the LRU ----------------

const char* RedisFileCache::LUA_CAN_EVICT() {
    return R"(
        -- KEYS[1]=write_lock  KEYS[2]=readers_count  KEYS[3]=evict_fence
        -- ARGV[1]=ttl_ms
        if redis.call('EXISTS', KEYS[1]) == 1 then return 0 end
        local rc = tonumber(redis.call('GET', KEYS[2]) or "0")
        if rc > 0 then return 0 end
        local ok = redis.call('SET', KEYS[3], '1', 'NX', 'PX', tonumber(ARGV[1]))
        if ok then return 1 else return 0 end
    )";
}

// ------- ctors -------
RedisFileCache::RedisFileCache(std::string cache_dir,
                               std::string redis_host,
                               int redis_port,
                               int redis_db,
                               long long lock_ttl_ms,
                               std::string ns)
: cache_dir_(std::move(cache_dir)),
  ns_(std::move(ns)),
  ttl_ms_(lock_ttl_ms),
  rc_(nullptr, rc_deleter)
{
    ensure_dir(cache_dir_);

    // connect
    redisContext* c = redisConnect(redis_host.c_str(), redis_port);
    if (!c || c->err) {
        std::string msg = c ? c->errstr : "redisConnect failed";
        if (c) redisFree(c);
        throw std::runtime_error("Redis connect error: " + msg);
    }
    rc_.reset(c);

    // select DB
    if (redis_db != 0) {
        // FIXME This throws std::runtime_error and if it does, the cache should be invalid.
        //  jhrg 9/30/25
        long long ok = cmd_ll("SELECT %d", redis_db);
        (void)ok; // This is bogus.
    }

    // load scripts
    sha_rl_acq_ = script_load(LUA_READ_LOCK_ACQUIRE());
    sha_rl_rel_ = script_load(LUA_READ_LOCK_RELEASE());
    sha_wl_acq_ = script_load(LUA_WRITE_LOCK_ACQUIRE());
    sha_wl_rel_ = script_load(LUA_WRITE_LOCK_RELEASE());
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
        std::string msg = c ? c->errstr : "redisConnect failed";
        if (c) redisFree(c);
        throw std::runtime_error("Redis connect error: " + msg);
    }
    rc_.reset(c);

    if (redis_db != 0) {
        // FIXME See above jhrg 9/30/25
        cmd_ll("SELECT %d", redis_db);
    }

    // load scripts
    sha_rl_acq_ = script_load(LUA_READ_LOCK_ACQUIRE());
    sha_rl_rel_ = script_load(LUA_READ_LOCK_RELEASE());
    sha_wl_acq_ = script_load(LUA_WRITE_LOCK_ACQUIRE());
    sha_wl_rel_ = script_load(LUA_WRITE_LOCK_RELEASE());

    sha_can_evict_ = script_load(LUA_CAN_EVICT());

    // index keys
    z_lru_ = ns_ + ":idx:lru";
    h_sizes_ = ns_ + ":idx:size";
    s_keys_ = ns_ + ":keys:set";
    k_total_ = ns_ + ":idx:total";
    k_purge_mtx_ = ns_ + ":purge:mutex";
}

// ------- hiredis helpers -------
long long RedisFileCache::cmd_ll(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    redisReply* r = (redisReply*)redisvCommand(rc_.get(), fmt, ap);
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

std::string RedisFileCache::cmd_s(const char* fmt, ...) {
    va_list ap; va_start(ap, fmt);
    redisReply* r = (redisReply*)redisvCommand(rc_.get(), fmt, ap);
    va_end(ap);
    if (!r) throw std::runtime_error("Redis command failed (NULL reply)");
    std::unique_ptr<redisReply, void(*)(void*)> guard(r, freeReplyObject);
    if (r->type == REDIS_REPLY_STRING || r->type == REDIS_REPLY_STATUS) {
        return std::string(r->str, r->len);
    }
    if (r->type == REDIS_REPLY_NIL) return {};
    throw std::runtime_error("Unexpected reply type (string expected)");
}

std::string RedisFileCache::script_load(const std::string& script) {
    redisReply* r = (redisReply*)redisCommand(rc_.get(), "SCRIPT LOAD %b", script.data(), (size_t)script.size());
    if (!r) throw std::runtime_error("SCRIPT LOAD failed");
    std::unique_ptr<redisReply, void(*)(void*)> guard(r, freeReplyObject);
    if (r->type != REDIS_REPLY_STRING) throw std::runtime_error("SCRIPT LOAD bad reply");
    return std::string(r->str, r->len);
}

long long RedisFileCache::evalsha_ll(const std::string& sha,
                                     int nkeys,
                                     const std::string* keys,
                                     int nargs,
                                     const std::string* args)
{
    // Construct argv for: EVALSHA sha nkeys key... arg...
    std::vector<const char*> argv;
    std::vector<size_t> lens;

    auto push = [&](const std::string& s){
        argv.push_back(s.data());
        lens.push_back(s.size());
    };

    std::string nkeys_s = std::to_string(nkeys);

    push("EVALSHA"); push(sha);
    push(nkeys_s);
    for (int i=0;i<nkeys;++i) push(keys[i]);
    for (int i=0;i<nargs;++i) push(args[i]);

    redisReply* rr = (redisReply*)redisCommandArgv(rc_.get(), (int)argv.size(), argv.data(), lens.data());
    if (!rr) throw std::runtime_error("EVALSHA failed");
    std::unique_ptr<redisReply, void(*)(void*)> guard(rr, freeReplyObject);

    if (rr->type == REDIS_REPLY_INTEGER) return rr->integer;
    if (rr->type == REDIS_REPLY_STRING) {
        try { return std::stoll(std::string(rr->str, rr->len)); }
        catch (...) { throw std::runtime_error("EVALSHA string->int parse error"); }
    }
    if (rr->type == REDIS_REPLY_NIL) return 0;
    if (rr->type == REDIS_REPLY_STATUS) return 1;
    throw std::runtime_error("EVALSHA: unexpected reply type");
}

// ------- locking -------
void RedisFileCache::acquire_read(const std::string& key) {
    std::string keys[2] = { k_write(key), k_readers(key) };
    std::string args[1] = { std::to_string(ttl_ms_) };
    long long res = evalsha_ll(sha_rl_acq_, 2, keys, 1, args);
    if (res != 1) throw CacheBusyError("read lock blocked by writer");
}
void RedisFileCache::release_read(const std::string& key) noexcept {
    try {
        std::string keys[1] = { k_readers(key) };
        evalsha_ll(sha_rl_rel_, 1, keys, 0, nullptr);
    } catch (...) {}
}

std::string RedisFileCache::acquire_write(const std::string& key) {
    // random token
    std::random_device rd; std::mt19937_64 g(rd());
    uint64_t a=g(), b=g();
    std::ostringstream oss;
    oss<<std::hex<<std::setw(16)<<std::setfill('0')<<a
       <<std::setw(16)<<std::setfill('0')<<b;
    std::string token = oss.str();

    std::string keys[2] = { k_write(key), k_readers(key) };
    std::string args[2] = { token, std::to_string(ttl_ms_) };
    long long res = evalsha_ll(sha_wl_acq_, 2, keys, 2, args);
    if (res == 0) throw CacheBusyError("writer lock held by another writer");
    if (res == -1) throw CacheBusyError("readers present");
    return token;
}
void RedisFileCache::release_write(const std::string& key, const std::string& token) noexcept {
    try {
        std::string keys[1] = { k_write(key) };
        std::string args[1] = { token };
        evalsha_ll(sha_wl_rel_, 1, keys, 1, args);
    } catch (...) {}
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
    long long sz = (long long)data.size();
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

bool RedisFileCache::try_evict_one(std::string& victim, long long& freed) {
    victim.clear(); freed = 0;

    // Oldest (lowest score) by LRU
    redisReply* r = (redisReply*)redisCommand(rc_.get(),
        "ZRANGE %s 0 0 WITHSCORES", z_lru_.c_str());
    if (!r) return false;
    std::unique_ptr<redisReply, void(*)(void*)> guard(r, freeReplyObject);
    if (r->type != REDIS_REPLY_ARRAY || r->elements < 1) return false;

    std::string key(r->element[0]->str, r->element[0]->len);

    // size lookup
    redisReply* rs = (redisReply*)redisCommand(rc_.get(),
        "HGET %s %b", h_sizes_.c_str(), key.data(), (size_t)key.size());
    if (!rs) return false;
    std::unique_ptr<redisReply, void(*)(void*)> guards(rs, freeReplyObject);
    if (rs->type == REDIS_REPLY_NIL) {
        // index drift; clean LRU entry and continue
        cmd_ll("ZREM %s %b", z_lru_.c_str(), key.data(), (size_t)key.size());
        cmd_ll("SREM %s %b", s_keys_.c_str(), key.data(), (size_t)key.size());
        return false;
    }
    long long sz = std::stoll(std::string(rs->str, rs->len));

    // Fence & verify evictable (no readers/writers)
    std::string keys[3] = { k_write(key), k_readers(key), k_evict_fence(ns_, key) };
    std::string args[1] = { std::to_string(1500) }; // 1.5s fence
    long long ok = evalsha_ll(sha_can_evict_, 3, keys, 1, args);
    if (ok != 1) {
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
