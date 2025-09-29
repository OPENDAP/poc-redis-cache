//
// Created by James Gallagher on 9/26/25.
//
#include "redis_poc_cache.h"

#include <sys/stat.h>
#include <sys/types.h>
#include <sys/file.h>
#include <fcntl.h>
#include <unistd.h>
#include <errno.h>
#include <string.h>

#include <sstream>
#include <random>
#include <iomanip>

using sw::redis::Redis;
using sw::redis::StringView;

static bool file_exists(const std::string& p) {
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0 && S_ISREG(st.st_mode);
}

void RedisFileCache::validate_key(const std::string& key) {
    if (key.empty() || key.front()=='.' || key.find('/') != std::string::npos) {
        throw std::invalid_argument("Key must be a simple filename");
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

const std::string& RedisFileCache::lua_read_lock_acquire() {
    static const std::string s = R"(
        local write_lock = KEYS[1]
        local readers = KEYS[2]
        local ttl = tonumber(ARGV[1])
        if redis.call('EXISTS', write_lock) == 1 then
            return 0
        end
        local c = redis.call('INCR', readers)
        redis.call('PEXPIRE', readers, ttl)
        return 1
    )";
    return s;
}
const std::string& RedisFileCache::lua_read_lock_release() {
    static const std::string s = R"(
        local readers = KEYS[1]
        local c = redis.call('DECR', readers)
        if c <= 0 then
            redis.call('DEL', readers)
        end
        return 1
    )";
    return s;
}
const std::string& RedisFileCache::lua_write_lock_acquire() {
    static const std::string s = R"(
        local write_lock = KEYS[1]
        local readers = KEYS[2]
        local token = ARGV[1]
        local ttl = tonumber(ARGV[2])
        if redis.call('EXISTS', write_lock) == 1 then
            return 0
        end
        local rc = tonumber(redis.call('GET', readers) or "0")
        if rc > 0 then
            return -1
        end
        local ok = redis.call('SET', write_lock, token, 'NX', 'PX', ttl)
        if ok then return 1 else return 0 end
    )";
    return s;
}
const std::string& RedisFileCache::lua_write_lock_release() {
    static const std::string s = R"(
        local write_lock = KEYS[1]
        local token = ARGV[1]
        local cur = redis.call('GET', write_lock)
        if cur and cur == token then
            redis.call('DEL', write_lock)
            return 1
        end
        return 0
    )";
    return s;
}

RedisFileCache::RedisFileCache(std::string cache_dir,
                               std::string redis_url,
                               long long lock_ttl_ms,
                               std::string ns)
: cache_dir_(std::move(cache_dir)),
  r_(new Redis(redis_url)),
  ttl_ms_(lock_ttl_ms),
  ns_(std::move(ns)) {

    ::mkdir(cache_dir_.c_str(), 0777);

    // Preload scripts; store SHAs
    sha_read_acq_  = r_->script_load(lua_read_lock_acquire());
    sha_read_rel_  = r_->script_load(lua_read_lock_release());
    sha_write_acq_ = r_->script_load(lua_write_lock_acquire());
    sha_write_rel_ = r_->script_load(lua_write_lock_release());
}

bool RedisFileCache::exists(const std::string& key) const {
    validate_key(key);
    return file_exists(path_for(key));
}

void RedisFileCache::fsync_fd(int fd) {
    if (::fsync(fd) != 0) {
        // Best-effort; throw if truly exceptional
        int e = errno;
        throw std::system_error(e, std::generic_category(), "fsync");
    }
}

void RedisFileCache::acquire_read(const std::string& key) {
    std::vector<std::string> keys{ k_write(key), k_readers(key) };
    std::vector<std::string> argv{ std::to_string(ttl_ms_) };
    auto res = r_->evalsha<long long>(sha_read_acq_, keys.begin(), keys.end(),
                                      argv.begin(), argv.end());
    if (res != 1) {
        throw CacheBusyError("read lock blocked by writer");
    }
}
void RedisFileCache::release_read(const std::string& key) noexcept {
    try {
        std::vector<std::string> keys{ k_readers(key) };
        std::vector<std::string> argv{ token };
        r_->evalsha<long long>(sha_read_rel_, keys.begin(), keys.end(),
                               argv.begin(), argv.end());
    } catch (...) {
        // swallow in noexcept
    }
}

std::string RedisFileCache::acquire_write(const std::string& key) {
    // Unique token: 128-bit random hex
    std::random_device rd;
    std::mt19937_64 gen(rd());
    uint64_t a = gen(), b = gen();
    std::ostringstream oss;
    oss << std::hex << std::setw(16) << std::setfill('0') << a
        << std::setw(16) << std::setfill('0') << b;
    std::string token = oss.str();

    std::vector<std::string> keys{ k_write(key), k_readers(key) };
    std::vector<std::string> argv{ token, std::to_string(ttl_ms_) };
    long long res = r_->evalsha<long long>(sha_write_acq_, keys.begin(), keys.end(),
                                           argv.begin(), argv.end());
    if (res == 0) throw CacheBusyError("writer lock held by another writer");
    if (res == -1) throw CacheBusyError("readers present");
    return token;
}
void RedisFileCache::release_write(const std::string& key, const std::string& token) noexcept {
    try {
        std::vector<std::string> keys{ k_write(key) };
        std::vector<std::string> argv{ token };
        r_->evalsha<long long>(sha_write_rel_, keys.begin(), keys.end(),
                               argv.begin(), argv.end());
    } catch (...) {}
}

std::string RedisFileCache::read_bytes(const std::string& key) {
    validate_key(key);
    const auto p = path_for(key);

    // Acquire lock, open, read, release
    acquire_read(key);
    int fd = -1;
    try {
        fd = ::open(p.c_str(), O_RDONLY);
        if (fd < 0) {
            int e = errno;
            release_read(key);
            if (e == ENOENT) throw std::system_error(e, std::generic_category(), "FileNotFound");
            throw std::system_error(e, std::generic_category(), "open for read");
        }

        std::string buf;
        const size_t CHUNK = 1 << 16;
        char tmp[CHUNK];
        ssize_t n;
        while ((n = ::read(fd, tmp, CHUNK)) > 0) buf.append(tmp, tmp + n);
        if (n < 0) {
            int e = errno;
            ::close(fd);
            release_read(key);
            throw std::system_error(e, std::generic_category(), "read");
        }
        ::close(fd);
        release_read(key);
        return buf;
    } catch (...) {
        if (fd >= 0) ::close(fd);
        // ensure release on exceptions
        release_read(key);
        throw;
    }
}

void RedisFileCache::write_bytes_create(const std::string& key, const std::string& data) {
    validate_key(key);
    const auto p = path_for(key);

    if (file_exists(p)) {
        throw std::system_error(EEXIST, std::generic_category(), "exists");
    }

    std::string token = acquire_write(key);

    // temp file in same dir for atomic rename
    char tmpl[PATH_MAX];
    std::snprintf(tmpl, sizeof(tmpl), "%s/.%s.XXXXXX", cache_dir_.c_str(), key.c_str());
    int tfd = ::mkstemp(tmpl);
    if (tfd < 0) {
        int e = errno;
        release_write(key, token);
        throw std::system_error(e, std::generic_category(), "mkstemp");
    }

    // write
    ssize_t total = 0;
    const char* ptr = data.data();
    ssize_t left = static_cast<ssize_t>(data.size());
    while (left > 0) {
        ssize_t n = ::write(tfd, ptr + total, left);
        if (n < 0) {
            int e = errno;
            ::close(tfd);
            ::unlink(tmpl);
            release_write(key, token);
            throw std::system_error(e, std::generic_category(), "write");
        }
        total += n;
        left -= n;
    }

    try { fsync_fd(tfd); } catch (...) {
        ::close(tfd);
        ::unlink(tmpl);
        release_write(key, token);
        throw;
    }
    ::close(tfd);

    // Final safety: enforce create-only just before publish
    if (file_exists(p)) {
        ::unlink(tmpl);
        release_write(key, token);
        throw std::system_error(EEXIST, std::generic_category(), "concurrent create");
    }

    if (::rename(tmpl, p.c_str()) != 0) {
        int e = errno;
        ::unlink(tmpl);
        release_write(key, token);
        throw std::system_error(e, std::generic_category(), "rename");
    }

    release_write(key, token);
}
