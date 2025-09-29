//
// Created by James Gallagher on 9/26/25.
//

#ifndef POC_REDIS_CACHE_REDIS_POC_CACHE_H
#define POC_REDIS_CACHE_REDIS_POC_CACHE_H

#include <sw/redis++/redis++.h>
#include <string>
#include <stdexcept>
#include <memory>
#include <functional>

class CacheBusyError : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
};

class RedisFileCache {
public:
    RedisFileCache(std::string cache_dir,
                   std::string redis_url = "tcp://127.0.0.1:6379",
                   long long lock_ttl_ms = 60000,
                   std::string ns = "poc-cache");

    // Non-copyable
    RedisFileCache(const RedisFileCache&) = delete;
    RedisFileCache& operator=(const RedisFileCache&) = delete;

    // Read entire file into memory with a read-lock
    std::string read_bytes(const std::string& key);

    // Create-only write: throws EEXIST if file already exists,
    // throws CacheBusyError if readers/writer present.
    void write_bytes_create(const std::string& key, const std::string& data);

    // Simple existence check
    bool exists(const std::string& key) const;

private:
    std::string cache_dir_;
    std::unique_ptr<sw::redis::Redis> r_;
    long long ttl_ms_;
    std::string ns_;

    // Lua SHA1s (loaded in ctor)
    std::string sha_read_acq_;
    std::string sha_read_rel_;
    std::string sha_write_acq_;
    std::string sha_write_rel_;

    static const std::string& lua_read_lock_acquire();
    static const std::string& lua_read_lock_release();
    static const std::string& lua_write_lock_acquire();
    static const std::string& lua_write_lock_release();

    std::string path_for(const std::string& key) const;
    std::string k_write(const std::string& key) const;
    std::string k_readers(const std::string& key) const;

    // RAII helpers
    void acquire_read(const std::string& key);
    void release_read(const std::string& key) noexcept;

    std::string acquire_write(const std::string& key); // returns token
    void release_write(const std::string& key, const std::string& token) noexcept;

    static void validate_key(const std::string& key);
    static void fsync_fd(int fd);
};

#endif //POC_REDIS_CACHE_REDIS_POC_CACHE_H