//
// Created by James Gallagher on 9/29/25.
//

#include "RedisFileCache.hpp"

#include <hiredis/hiredis.h>
#include <hiredis/async.h>

#include <sys/stat.h>
#include <sys/types.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>

#include <cstring>
#include <cstdarg>
#include <vector>
#include <sstream>
#include <random>
#include <iomanip>

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

// ------- ctor -------
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
        long long ok = cmd_ll("SELECT %d", redis_db);
        (void)ok;
    }

    // load scripts
    sha_rl_acq_ = script_load(LUA_READ_LOCK_ACQUIRE());
    sha_rl_rel_ = script_load(LUA_READ_LOCK_RELEASE());
    sha_wl_acq_ = script_load(LUA_WRITE_LOCK_ACQUIRE());
    sha_wl_rel_ = script_load(LUA_WRITE_LOCK_RELEASE());
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
}
