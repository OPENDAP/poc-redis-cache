//
// Created by James Gallagher on 9/29/25.
//

#ifndef POC_REDIS_CACHE_REDIS_POC_CACHE_HIREDIS_H
#define POC_REDIS_CACHE_REDIS_POC_CACHE_HIREDIS_H

#include <string>
#include <stdexcept>
#include <memory>
#include <chrono>

struct redisContext;
struct redisReply;

class CacheBusyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RedisFileCache {
public:
    explicit RedisFileCache(std::string cache_dir,
                   std::string redis_host = "127.0.0.1",
                   int redis_port = 6379,
                   int redis_db = 0,
                   long long lock_ttl_ms = 60000,
                   std::string ns = "poc-cache");

    // Non-copyable
    RedisFileCache(const RedisFileCache&) = delete;
    RedisFileCache& operator=(const RedisFileCache&) = delete;

    // Read all bytes with a read-lock (non-blocking; throws CacheBusyError if writer present)
    std::string read_bytes(const std::string& key);

    // Create-only write (non-blocking; throws EEXIST or CacheBusyError)
    void write_bytes_create(const std::string& key, const std::string& data);

    bool exists(const std::string& key) const;

    // Expose namespace for test convenience
    const std::string& namespace_prefix() const { return ns_; }

private:
    // -------- config / state --------
    std::string cache_dir_;
    std::string ns_;
    long long ttl_ms_;
    std::unique_ptr<redisContext, void(*)(redisContext*)> rc_;

    // Lua SHAs
    std::string sha_rl_acq_, sha_rl_rel_, sha_wl_acq_, sha_wl_rel_;

    // -------- internal helpers --------
    static void validate_key(const std::string& key);
    std::string path_for(const std::string& key) const;
    std::string k_write(const std::string& key) const;
    std::string k_readers(const std::string& key) const;

    // hiredis helpers
    long long cmd_ll(const char* fmt, ...);         // integer reply
    std::string cmd_s(const char* fmt, ...);        // bulk/str reply
    std::string script_load(const std::string& script);
    long long evalsha_ll(const std::string& sha,
                         int nkeys,
                         const std::string* keys,
                         int nargs,
                         const std::string* args);

    // Lua sources
    static const char* LUA_READ_LOCK_ACQUIRE();
    static const char* LUA_READ_LOCK_RELEASE();
    static const char* LUA_WRITE_LOCK_ACQUIRE();
    static const char* LUA_WRITE_LOCK_RELEASE();

    // lock ops
    void acquire_read(const std::string& key);
    void release_read(const std::string& key) noexcept;

    std::string acquire_write(const std::string& key);
    void release_write(const std::string& key, const std::string& token) noexcept;

    // file ops
    static bool file_exists_(const std::string& p);
    static void fsync_fd(int fd);
};


#endif //POC_REDIS_CACHE_REDIS_POC_CACHE_HIREDIS_H